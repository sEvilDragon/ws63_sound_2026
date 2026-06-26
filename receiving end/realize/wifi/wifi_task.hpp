#include "dlan.hpp"
#include "iis.hpp"
#include "minimp3.hpp"
#include "provisioner.hpp"
#include "http_control.hpp"

void *wifi_task(void *arg);
void *dlna_task(void *arg);
void *minimp3_task(void *arg);
