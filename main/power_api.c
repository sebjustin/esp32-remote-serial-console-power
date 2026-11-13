#include "power_api.h"
#include "power_control.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

static const char *TAG = "power_api";
static const char PAGE[] = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Remote Power</title><style>body{font:16px sans-serif;max-width:560px;margin:30px auto;padding:0 16px;background:#111;color:#eee}button,input{font-size:18px;padding:12px;margin:5px;border-radius:8px}button{cursor:pointer}.danger{background:#a22;color:white}#state{font-size:24px;padding:15px;background:#333;border-radius:8px}</style></head><body><h2>ESP32 Remote Power</h2><div id='state'>Status: ...</div><p><label>Hasło: <input id='pw' type='password' autocomplete='current-password'></label></p><button onclick=\"cmd('ON')\">POWER ON (1 s)</button><button onclick=\"cmd('OFF')\">POWER OFF (1 s)</button><button class='danger' onclick=\"force()\">FORCE OFF (5 s)</button><p id='msg'></p><script>async function api(c){let r=await fetch('/api',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'password='+encodeURIComponent(document.getElementById('pw').value)+'&cmd='+encodeURIComponent(c)});let t=await r.text();document.getElementById('msg').textContent=t;if(r.ok)status()}async function cmd(c){api(c)}function force(){if(confirm('Wymusić wyłączenie? Niezapisane dane mogą zostać utracone.'))api('FORCE')}async function status(){try{let r=await fetch('/status');let j=await r.json();document.getElementById('state').textContent='Komputer: '+j.state+(j.busy?' (przekaźnik aktywny)':'')}catch(e){document.getElementById('state').textContent='Brak połączenia'}}setInterval(status,2000);status();</script></body></html>";

static esp_err_t root_get(httpd_req_t *r) { httpd_resp_set_type(r,"text/html; charset=utf-8"); return httpd_resp_send(r,PAGE,HTTPD_RESP_USE_STRLEN); }
static esp_err_t status_get(httpd_req_t *r) {
    char b[96]; snprintf(b,sizeof(b),"{\"state\":\"%s\",\"busy\":%s}", power_control_is_on()?"ON":"OFF", power_control_is_busy()?"true":"false");
    httpd_resp_set_type(r,"application/json"); return httpd_resp_sendstr(r,b);
}
static esp_err_t api_post(httpd_req_t *r) {
    char body[256]={0}; int n=r->content_len;
    if(n<=0 || n>=sizeof(body)) { httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid body"); return ESP_FAIL; }
    int got=httpd_req_recv(r,body,n); if(got<=0){httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Read failed");return ESP_FAIL;} body[got]=0;
    char *pw=strstr(body,"password="); char *cmd=strstr(body,"cmd=");
    if(!pw || !cmd){httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Missing fields");return ESP_FAIL;}
    pw+=9; char *amp=strchr(pw,'&'); if(amp)*amp=0; cmd+=4;
    /* URL decode only '+' and %xx for password/cmd. */
    char pass[80]={0}, command[24]={0};
    for(size_t i=0,j=0;pw[i]&&j<sizeof(pass)-1;i++,j++){ if(pw[i]=='+')pass[j]=' '; else if(pw[i]=='%'&&pw[i+1]&&pw[i+2]){char h[3]={pw[i+1],pw[i+2],0};pass[j]=(char)strtol(h,NULL,16);i+=2;}else pass[j]=pw[i]; }
    for(size_t i=0,j=0;cmd[i]&&j<sizeof(command)-1;i++,j++){ if(cmd[i]=='+')command[j]=' '; else if(cmd[i]=='%'&&cmd[i+1]&&cmd[i+2]){char h[3]={cmd[i+1],cmd[i+2],0};command[j]=(char)strtol(h,NULL,16);i+=2;}else command[j]=cmd[i]; }
    if(strcmp(pass,g_config.sespass)!=0 || g_config.sespass[0]=='\0'){httpd_resp_set_status(r,"401 Unauthorized");httpd_resp_sendstr(r,"Unauthorized");return ESP_FAIL;}
    uint32_t ms=0;
    if(!strcmp(command,"ON")||!strcmp(command,"OFF")) ms=1000;
    else if(!strcmp(command,"FORCE")) { if(power_control_is_on()) ms=5000; else {httpd_resp_sendstr(r,"Already OFF");return ESP_OK;} }
    else {httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Unknown command");return ESP_FAIL;}
    if(!strcmp(command,"ON") && power_control_is_on()){httpd_resp_sendstr(r,"Already ON");return ESP_OK;}
    if(!strcmp(command,"OFF") && !power_control_is_on()){httpd_resp_sendstr(r,"Already OFF");return ESP_OK;}
    esp_err_t e=power_control_pulse(ms);
    if (e != ESP_OK) {
    httpd_resp_set_status(r, "409 Conflict");
    httpd_resp_sendstr(r, "Relay busy or invalid request");
    return ESP_OK;
}
return ESP_OK;
}
static void http_start(void) {
    httpd_config_t c=HTTPD_DEFAULT_CONFIG(); c.server_port=80; c.max_uri_handlers=8;
    httpd_handle_t h=NULL; if(httpd_start(&h,&c)!=ESP_OK){ESP_LOGE(TAG,"HTTP start failed");return;}
    httpd_uri_t a={.uri="/",.method=HTTP_GET,.handler=root_get}; httpd_register_uri_handler(h,&a);
    httpd_uri_t b={.uri="/status",.method=HTTP_GET,.handler=status_get}; httpd_register_uri_handler(h,&b);
    httpd_uri_t d={.uri="/api",.method=HTTP_POST,.handler=api_post}; httpd_register_uri_handler(h,&d);
}
static void tcp_control_task(void *arg) {
    int s=socket(AF_INET,SOCK_STREAM,IPPROTO_IP); if(s<0){vTaskDelete(NULL);return;}
    int yes=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(2323),.sin_addr.s_addr=htonl(INADDR_ANY)};
    if(bind(s,(struct sockaddr*)&a,sizeof(a))<0||listen(s,2)<0){ESP_LOGE(TAG,"TCP control bind/listen failed");close(s);vTaskDelete(NULL);return;}
    ESP_LOGI(TAG,"Power command server listening on TCP 2323");
    while(1){int c=accept(s,NULL,NULL);if(c<0)continue; struct timeval tv={.tv_sec=5};setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
        char line[160]={0}; int n=recv(c,line,sizeof(line)-1,0); if(n>0){line[n]=0; char *nl=strpbrk(line,"\r\n");if(nl)*nl=0;
            char *sp=strchr(line,' '); if(sp){*sp++=0; if(!strcmp(line,"PASS")){char *cmdline=strchr(sp,' '); if(cmdline){*cmdline++=0; if(!strcmp(sp,g_config.sespass)&&g_config.sespass[0]){
                uint32_t ms=0; if(!strcmp(cmdline,"STATUS")){char out[64];snprintf(out,sizeof(out),"%s\r\n",power_control_is_on()?"ON":"OFF");send(c,out,strlen(out),0);} else if(!strcmp(cmdline,"POWER ON")||!strcmp(cmdline,"POWER OFF"))ms=1000; else if(!strcmp(cmdline,"POWER FORCE")&&power_control_is_on())ms=5000;
                if(ms){esp_err_t e=power_control_pulse(ms);send(c,e==ESP_OK?"OK\r\n":"BUSY/ERROR\r\n",e==ESP_OK?4:12,0);} else if(strcmp(cmdline,"STATUS"))send(c,"ERR command/state\r\n",19,0);
            }else send(c,"ERR auth\r\n",10,0);}}else send(c,"ERR use: PASS <password> <command>\r\n",37,0);}else send(c,"ERR use: PASS <password> <command>\r\n",37,0);
        }close(c);
    }
}
void power_api_start(void){ power_control_init(); http_start(); if(xTaskCreate(tcp_control_task,"power_tcp",4096,NULL,4,NULL)!=pdPASS)ESP_LOGE(TAG,"TCP task create failed"); }
