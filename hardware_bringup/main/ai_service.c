#include "ai_service.h"

#include <stdio.h>

#include "xiaozhi_client.h"

void ai_service_start_session(void)
{
    xiaozhi_client_handle_boot_button();
}

void ai_service_stop_session(void)
{
    xiaozhi_client_stop_session();
}
