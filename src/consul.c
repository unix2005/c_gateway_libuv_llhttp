#include "service.h"

int register_to_consul() 
{
  CURL *curl = curl_easy_init();
  if(!curl) return -1;

  struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
  const char* data = "{\"Name\": \"c-api-service\", \"Port\": 8080}";

  curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:8500/v1/agent/service/register");
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");

  CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  return (res == CURLE_OK) ? 0 : -1;
}
