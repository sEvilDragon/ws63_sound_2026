#ifndef __AUDIO_PLAY_HPP__
#define __AUDIO_PLAY_HPP__

#include "sle.hpp"
#include "iis.hpp"

void *audio_play_task(void *arg);
bool audio_sle_task_running(void);

#endif
