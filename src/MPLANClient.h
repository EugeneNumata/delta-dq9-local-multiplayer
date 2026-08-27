/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "melonDS/src/types.h"

namespace MPLAN
{
bool Init();
void DeInit();
void Begin();
void End();
int SendPacket(u8* data, int len, u64 timestamp);
int RecvPacket(u8* data, u64* timestamp);
int SendCmd(u8* data, int len, u64 timestamp);
int SendReply(u8* data, int len, u64 timestamp, u16 aid);
int SendAck(u8* data, int len, u64 timestamp);
int RecvHostPacket(u8* data, u64* timestamp);
u16 RecvReplies(u8* data, u64 timestamp, u16 aidmask);
}
