#include "ai_service.h"

#include <stdio.h>

#include "audio_service.h"
#include "xiaozhi_client.h"

void ai_service_start_session(void)
{
    if (xiaozhi_client_start_session()) {
        printf("ai: official session requested\n");
    }
}

void ai_service_stop_session(void)
{
    xiaozhi_client_stop_session();
    audio_service_stop_session();
}
