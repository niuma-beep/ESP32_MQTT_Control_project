#include "dns_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "dns_server";

typedef struct __attribute__((packed))
{
  uint16_t id;
  uint16_t flags;
  uint16_t qd_count;
  uint16_t an_count;
  uint16_t ns_count;
  uint16_t ar_count;
} dns_header_t;

typedef struct __attribute__((packed))
{
  uint16_t type;
  uint16_t class;
} dns_question_t;

typedef struct __attribute__((packed))
{
  uint16_t ptr_offset;
  uint16_t type;
  uint16_t class;
  uint32_t ttl;
  uint16_t addr_len;
  uint32_t ip_addr;
} dns_answer_t;

static bool parse_dns_name(const char *msg, int msg_len, const char *name,
                           const char **name_end, char *out,
                           size_t out_size)
{
  const char *end = msg + msg_len;
  const char *p = name;
  size_t out_pos = 0;

  if (!msg || !name || !name_end || !out || out_size == 0)
  {
    return false;
  }

  out[0] = '\0';
  while (p < end)
  {
    uint8_t label_len = (uint8_t)*p++;
    if (label_len == 0)
    {
      *name_end = p;
      return true;
    }
    if ((label_len & 0xC0) != 0 || p + label_len > end)
    {
      return false;
    }

    if (out_pos > 0 && out_pos < out_size - 1)
    {
      out[out_pos++] = '.';
    }
    size_t copy_len = label_len;
    if (copy_len > out_size - 1 - out_pos)
    {
      copy_len = out_size - 1 - out_pos;
    }
    memcpy(out + out_pos, p, copy_len);
    out_pos += copy_len;
    out[out_pos] = '\0';
    p += label_len;
  }

  return false;
}

static int build_dns_reply(char *req, int req_len, char *reply, int reply_size,
                           uint32_t ip_addr)
{
  if (req_len <= (int)sizeof(dns_header_t) || req_len > reply_size)
  {
    return -1;
  }

  memcpy(reply, req, req_len);
  dns_header_t *header = (dns_header_t *)reply;
  uint16_t qd_count = ntohs(header->qd_count);
  header->flags = htons(0x8180);

  char *question_ptr = reply + sizeof(dns_header_t);
  char *answer_ptr = reply + req_len;
  uint16_t answer_count = 0;

  for (int i = 0; i < qd_count; i++)
  {
    char domain[128];
    const char *name_end_const = NULL;
    char *question_start = question_ptr;
    if (!parse_dns_name(reply, req_len, question_ptr, &name_end_const,
                        domain, sizeof(domain)))
    {
      return -1;
    }

    char *name_end = (char *)name_end_const;
    if (name_end + sizeof(dns_question_t) > reply + req_len ||
        answer_ptr + sizeof(dns_answer_t) > reply + reply_size)
    {
      return -1;
    }

    dns_question_t *question = (dns_question_t *)name_end;
    uint16_t qtype = ntohs(question->type);
    uint16_t qclass = ntohs(question->class);
    ESP_LOGI(TAG, "DNS query domain=%s type=%u class=%u", domain,
             (unsigned)qtype, (unsigned)qclass);

    if (qtype == 1 && qclass == 1)
    {
    dns_answer_t *answer = (dns_answer_t *)answer_ptr;
      answer->ptr_offset = htons(0xC000 | (question_start - reply));
    answer->type = question->type;
    answer->class = question->class;
    answer->ttl = htonl(60);
    answer->addr_len = htons(4);
    answer->ip_addr = ip_addr;

    answer_ptr += sizeof(dns_answer_t);
      answer_count++;
    }
    question_ptr = name_end + sizeof(dns_question_t);
  }

  header->an_count = htons(answer_count);
  return answer_ptr - reply;
}

void dns_server_task(void *arg)
{
  esp_netif_t *ap_netif = arg;
  esp_netif_ip_info_t ip_info;
  ESP_ERROR_CHECK(esp_netif_get_ip_info(ap_netif, &ip_info));

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0)
  {
    ESP_LOGE(TAG, "DNS socket failed: errno %d", errno);
    vTaskDelete(NULL);
  }

  struct sockaddr_in server_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(DNS_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
  {
    ESP_LOGE(TAG, "DNS bind failed: errno %d", errno);
    close(sock);
    vTaskDelete(NULL);
  }

  ESP_LOGI(TAG, "DNS redirect server started");
  while (1)
  {
    char rx[DNS_MAX_LEN];
    char tx[DNS_MAX_LEN];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    int len = recvfrom(sock, rx, sizeof(rx), 0,
                       (struct sockaddr *)&source_addr, &socklen);
    if (len < 0)
    {
      continue;
    }
    int tx_len = build_dns_reply(rx, len, tx, sizeof(tx), ip_info.ip.addr);
    if (tx_len > 0)
    {
      sendto(sock, tx, tx_len, 0, (struct sockaddr *)&source_addr,
             socklen);
    }
  }
}
