/* Focus 3 support, 2026. The Focus 3 sequence is adapted from
 * Kirisame-Nanoha/Lip_Camera_IP_Server, tracker_control.py (ec070f1).
 * Copyright DragonDreams GmbH 2024. MIT notice: THIRD_PARTY_NOTICES.md.
 * Original VFT sequence derives from vft-stream/Baballonia; see LICENSE.
 */
#include "tracker.h"
#include <string.h>

enum tracker_profile tracker_identify(uint16_t vid, uint16_t pid) {
  if (vid != HTC_VID) return TRACKER_AUTO;
  if (pid == VFT_PID) return TRACKER_VFT;
  if (pid == FOCUS3_PID) return TRACKER_FOCUS3;
  return TRACKER_AUTO;
}

const char *tracker_name(enum tracker_profile profile) {
  return profile == TRACKER_FOCUS3 ? "focus3" : profile == TRACKER_VFT ? "vft" : "auto";
}

static int send_command(const struct tracker_io *io, const uint8_t *command,
                        size_t size, int ack) {
  uint8_t buffer[TRACKER_MAX_CONTROL] = {0};
  memcpy(buffer, command, size);
  return io->send(io->context, buffer, io->length, ack);
}

static int stream(const struct tracker_io *io, int on, int ack) {
  const uint8_t command[] = {0x50, 0x14, 0x00, on ? 0x01 : 0x00};
  return send_command(io, command, sizeof command, ack);
}

static int magic(const struct tracker_io *io) {
  uint8_t command[TRACKER_MAX_CONTROL] = {0x51, 0x52};
  if (io->length >= 256) { command[254] = 0x53; command[255] = 0x54; }
  return io->send(io->context, command, io->length, 1);
}

static int sensor(const struct tracker_io *io, uint8_t addr, uint8_t value) {
  const uint8_t command[] = {0x50, 0xab, 0x60, 0x01, 0x01, 0, 0, 0, addr,
                            0x90, 0x01, 0x00, 0x01, 0, 0, 0, value};
  return send_command(io, command, sizeof command, 1);
}

static int focus3_ir(const struct tracker_io *io, int on) {
  /* System register 0x80182246, value 0x11=on / 0x03=off.
   * These commands are SET_CUR only: do not poll GET_CUR for an echo. */
  const uint8_t command[] = {0x50, 0xa2, 0x00, 0x04, 0x01, 0x80, 0x18,
                            0x22, 0x46, 0, 0x01, 0, 0, 0, 0, 0, on ? 0x11 : 0x03};
  return send_command(io, command, sizeof command, 0);
}

int tracker_set_state(enum tracker_profile profile, int on, const struct tracker_io *io) {
  if (!io || !io->send || !io->sleep_ms || io->length < 17 ||
      io->length > TRACKER_MAX_CONTROL) return -1;
  if (profile == TRACKER_FOCUS3) {
    if (!on) {
      /* Attempt both shutdown commands even if one transfer fails. */
      int ir_error = focus3_ir(io, 0);
      io->sleep_ms(io->context, 250);
      int stream_error = stream(io, 0, 0);
      io->sleep_ms(io->context, 250);
      return ir_error ? ir_error : stream_error;
    }
    int r = stream(io, 0, 0);
    if (r) return r;
    io->sleep_ms(io->context, 250);
    if ((r = stream(io, 1, 0))) return r;
    io->sleep_ms(io->context, 250);
    if ((r = focus3_ir(io, 1))) return r;
    io->sleep_ms(io->context, 250);
    return 0;
  }
  if (profile != TRACKER_VFT || (io->length != 64 && io->length != 384)) return -1;
  int r;
  if ((r = magic(io)) || (r = stream(io, 0, 1))) return r;
  io->sleep_ms(io->context, 100);
  if ((r = magic(io))) return r;
  const uint8_t addresses[] = {0x00, 0x08, 0x70, 0x02, 0x03, 0x04, 0x0e, 0x05, 0x06, 0x07, 0x0f};
  const uint8_t values[] = {0x40, 0x01, 0x00, on ? 0xff : 0, on ? 0xff : 0,
                           on ? 0xff : 0, 0x00, 0xb2, 0xb2, 0xb2, 0x03};
  for (size_t i = 0; i < sizeof addresses; i++)
    if ((r = sensor(io, addresses[i], values[i]))) return r;
  if (on) {
    io->sleep_ms(io->context, 100);
    if ((r = magic(io)) || (r = stream(io, 1, 1))) return r;
  }
  return 0;
}
