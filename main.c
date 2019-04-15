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
#define MUSIC_URL  "http://open.tyst.migu.cn/public/product5th/product30/2019/03/21/2018%E5%B9%B410%E6%9C%8811%E6%97%A521%E7%82%B925%E5%88%86%E6%89%B9%E9%87%8F%E9%A1%B9%E7%9B%AE%E5%8D%8E%E7%BA%B311%E9%A6%96-2/%E6%A0%87%E6%B8%85%E9%AB%98%E6%B8%85/MP3_128_16_Stero/6005751GYK6.mp3?channelid=08&msisdn=ca3af03b-9e8f-4fd5-840d-4216ebb45bc2&k=103f8806a79d9c04&t=1554276821171"

int main() {
  AudioParam param;
  param.channels = 1;
  param.rate = 16000;
  param.bit = 16;
  if (0 != Mp3Init(&param)) {
    LOGE(MAIN_TAG, "mp3 init failed");
    return -1;
  }
  Mp3Play(MUSIC_URL);
  while (1) {
    sleep(1);
  }
  return 0;
}
