#include "tracker.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct event { uint8_t bytes[384]; size_t length; int ack; unsigned delay; };
struct recorder { struct event events[64]; int count, writes, fail_write; };
static int send_fixture(void *context, const uint8_t *bytes, size_t length, int ack) {
  struct recorder *r = context;
  assert(r->count < 64 && length <= 384);
  struct event *e = &r->events[r->count++];
  memcpy(e->bytes, bytes, length); e->length = length; e->ack = ack;
  return ++r->writes == r->fail_write ? -7 : 0;
}
static void delay_fixture(void *context, unsigned delay) {
  struct recorder *r = context;
  assert(r->count < 64);
  r->events[r->count++].delay = delay;
}
static void expect_command(struct event *e, const uint8_t *bytes, size_t length, size_t payload) {
  assert(e->length == payload && !e->ack);
  assert(!memcmp(e->bytes, bytes, length));
  for (size_t i = length; i < payload; i++) assert(e->bytes[i] == 0);
}
int main(void) {
  assert(tracker_identify(HTC_VID, 0x06a1) == TRACKER_FOCUS3);
  assert(tracker_identify(HTC_VID, 0x0321) == TRACKER_VFT);
  assert(tracker_identify(HTC_VID, 0x9999) == TRACKER_AUTO);
  assert(tracker_identify(0x1234, 0x06a1) == TRACKER_AUTO);
  const uint8_t off[] = {0x50, 0x14, 0, 0}, on[] = {0x50, 0x14, 0, 1};
  const uint8_t ir_on[] = {0x50, 0xa2, 0, 4, 1, 0x80, 0x18, 0x22, 0x46, 0, 1, 0, 0, 0, 0, 0, 0x11};
  const uint8_t ir_off[] = {0x50, 0xa2, 0, 4, 1, 0x80, 0x18, 0x22, 0x46, 0, 1, 0, 0, 0, 0, 0, 0x03};
  for (size_t payload = 64; payload <= 384; payload += 320) {
    struct recorder r = {0};
    struct tracker_io io = {&r, payload, send_fixture, delay_fixture};
    assert(!tracker_set_state(TRACKER_FOCUS3, 1, &io));
    assert(r.count == 6);
    expect_command(&r.events[0], off, sizeof off, payload);
    expect_command(&r.events[2], on, sizeof on, payload);
    expect_command(&r.events[4], ir_on, sizeof ir_on, payload);
    assert(r.events[1].delay == 250 && r.events[3].delay == 250 && r.events[5].delay == 250);
    memset(&r, 0, sizeof r);
    assert(!tracker_set_state(TRACKER_FOCUS3, 0, &io));
    assert(r.count == 4);
    expect_command(&r.events[0], ir_off, sizeof ir_off, payload);
    expect_command(&r.events[2], off, sizeof off, payload);
    assert(r.events[1].delay == 250 && r.events[3].delay == 250);
    memset(&r, 0, sizeof r); r.fail_write = 1;
    assert(tracker_set_state(TRACKER_FOCUS3, 0, &io) == -7);
    assert(r.writes == 2); /* Stream-off still sent after an IR-off error. */
    for (int failure = 1; failure <= 3; failure++) {
      memset(&r, 0, sizeof r); r.fail_write = failure;
      assert(tracker_set_state(TRACKER_FOCUS3, 1, &io) == -7);
      assert(r.writes == failure); /* No further activation writes on failure. */
    }
    memset(&r, 0, sizeof r);
    assert(!tracker_set_state(TRACKER_VFT, 1, &io));
    assert(r.writes == 16 && r.count == 18);
    assert(r.events[0].bytes[0] == 0x51 && r.events[0].bytes[1] == 0x52);
    if (payload == 384) assert(r.events[0].bytes[254] == 0x53 && r.events[0].bytes[255] == 0x54);
    for (int i = 0; i < r.count; i++) if (r.events[i].length) assert(r.events[i].ack);
    const uint8_t addresses[] = {0, 8, 0x70, 2, 3, 4, 0x0e, 5, 6, 7, 0x0f};
    const uint8_t values[] = {0x40, 1, 0, 255, 255, 255, 0, 0xb2, 0xb2, 0xb2, 3};
    for (int i = 0; i < 11; i++) {
      struct event *e = &r.events[i + 4];
      assert(e->bytes[1] == 0xab && e->bytes[8] == addresses[i] && e->bytes[16] == values[i]);
    }
    memset(&r, 0, sizeof r);
    assert(!tracker_set_state(TRACKER_VFT, 0, &io));
    assert(r.writes == 14);
    for (int i = 3; i <= 5; i++) assert(r.events[i + 4].bytes[16] == 0);
  }
  struct recorder r = {0};
  struct tracker_io io = {&r, 16, send_fixture, delay_fixture};
  assert(tracker_set_state(TRACKER_FOCUS3, 1, &io));
  io.length = TRACKER_MAX_CONTROL + 1;
  assert(tracker_set_state(TRACKER_FOCUS3, 1, &io));
  io.length = 64;
  assert(tracker_set_state(TRACKER_AUTO, 1, &io));
  assert(!r.writes);
  puts("tracker: detection, activation/shutdown, padding, no-echo writes, failures, and VFT regression passed");
}
