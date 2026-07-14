#include "ai_service.h"

#include <stdio.h>

#include "audio_service.h"

void ai_service_start_session(void)
{
    const esp_err_t err = audio_service_start_session();
    printf("ai: session requested, audio=%s\n", esp_err_to_name(err));
}

void ai_service_stop_session(void)
{
    audio_service_stop_session();
}
