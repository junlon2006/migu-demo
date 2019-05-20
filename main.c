/**************************************************************************
 * Copyright (C) 2018-2019  Junlon2006
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 **************************************************************************
 *
 * Description : main.c
 * Author      : junlon2006@163.com
 * Date        : 2019.04.02
 *
 **************************************************************************/

#include "uni_mp3_player.h"
#include "uni_log.h"
#include <unistd.h>

#define MAIN_TAG   "main"

int main(int argc, char *argv[]) {
  AudioParam param;
  int count = 0;
  param.channels = 1;
  param.rate = 16000;
  param.bit = 16;
  if (argc != 2) {
    LOGE(MAIN_TAG, "argv invalid");
    return -1;
  }
  if (0 != Mp3Init(&param)) {
    LOGE(MAIN_TAG, "mp3 init failed");
    return -1;
  }
RE_START:
  if (count == 100) {
    goto L_END;
  }
  LOGT(MAIN_TAG, "begin to play %s", argv[1]);
  extern int retrieve_done;
  retrieve_done = 0;
  Mp3Play(argv[1]);
  while (1) {
    if (retrieve_done == 1) {
      Mp3Stop();
      LOGT(MAIN_TAG, "### retrieve_done[%d] ###", ++count);
      usleep(1000 * 1000);
      goto RE_START;
    }
    usleep(1000 * 100);
  }
L_END:
  usleep(1000 * 100);
  return 0;
}
