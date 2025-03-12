#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void start_webserver(void);

// app_main tarafındaki fonksiyonlar
void request_enroll(const char* name);
void request_save_user(void);
void request_delete(int user_id);
// void esp_err_t users_get_handler(httpd_req_t *req)


#ifdef __cplusplus
}
#endif
