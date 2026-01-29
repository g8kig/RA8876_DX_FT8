/*
 * decode_ft8.c
 *
 *  Created on: Sep 16, 2019
 *      Author: user
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

#include <RA8876_t3.h>
#include <Audio.h>
#include <si5351.h>

#include <TimeLib.h>

#include "gen_ft8.h"
#include "unpack.h"
#include "ldpc.h"
#include "decode.h"
#include "constants.h"
#include "encode.h"
#include "Process_DSP.h"
#include "display.h"
#include "decode_ft8.h"
#include "ADIF.h"
#include "button.h"
#include "main.h"
#include "traffic_manager.h"
#include "Geodesy.h"
#include "PskInterface.h"
#include "autoseq_engine.h"

int blank_length = 26;

const int kLDPC_iterations = 20;
const int kMax_candidates = 20;
const int kMax_decoded_messages = 20;
size_t kMax_message_length = 20;
const int kMin_score = 40; // Minimum sync score threshold for candidates

Decode new_decoded[20];

static const char *blank = "                      "; // 22 spaces
static const char *auto_blank = "             ";     // 14 spaces
static char worked_qso_entries[MAX_QSO_ENTRIES][MAX_LINE_LEN] = {};
static int num_qsos = 0;

static int validate_locator(const char *QSO_locator);

const int auto_call_limit = 10;
const int auto_logged_limit = 100;

int max_sync_score;
int max_sync_score_index;
Called_Stations call_list[auto_call_limit];
Called_Stations auto_logged_list[auto_logged_limit];

int auto_logged;
int Valid_CQ_Candidate;

int ft8_decode(void)
{
  // Find top candidates by Costas sync score and localize them in time and frequency
  Candidate candidate_list[kMax_candidates];

  int num_candidates = find_sync(export_fft_power, ft8_msg_samples, ft8_buffer, kCostas_map, kMax_candidates, candidate_list, kMin_score);
  char decoded[kMax_decoded_messages][kMax_message_length];

  const float fsk_dev = 6.25f; // tone deviation in Hz and symbol rate

  // Go over candidates and attempt to decode messages
  int num_decoded = 0;

  for (int idx = 0; idx < num_candidates; ++idx)
  {
    Candidate cand = candidate_list[idx];
    float freq_hz = (cand.freq_offset + cand.freq_sub / 2.0f) * fsk_dev;

    float log174[N];
    extract_likelihood(export_fft_power, ft8_buffer, cand, kGray_map, log174);

    // bp_decode() produces better decodes, uses way less memory
    uint8_t plain[N];
    int n_errors = 0;
    bp_decode(log174, kLDPC_iterations, plain, &n_errors);

    if (n_errors > 0)
      continue;

    // Extract payload + CRC (first K bits)
    uint8_t a91[K_BYTES];
    pack_bits(plain, K, a91);

    // Extract CRC and check it
    uint16_t chksum = ((a91[9] & 0x07) << 11) | (a91[10] << 3) | (a91[11] >> 5);
    a91[9] &= 0xF8;
    a91[10] = 0;
    a91[11] = 0;
    uint16_t chksum2 = crc(a91, 96 - 14);
    if (chksum != chksum2)
      continue;

    char message[kMax_message_length];

    char call_to[14];
    char call_from[14];
    char locator[7];
    int rc = unpack77_fields(a91, call_to, call_from, locator);
    if (rc < 0)
      continue;

    sprintf(message, "%s %s %s ", call_to, call_from, locator);

    // Check for duplicate messages (TODO: use hashing)
    bool found = false;
    for (int i = 0; i < num_decoded; ++i)
    {
      if (0 == strcmp(decoded[i], message))
      {
        found = true;
        break;
      }
    }

    int raw_RSL;
    int display_RSL;
    int received_RSL;

    getTeensy3Time();
    char rtc_string[10]; // print format stuff
    sprintf(rtc_string, "%02i%02i%02i", hour(), minute(), second());

    if (!found && num_decoded < kMax_decoded_messages)
    {
      if (strlen(message) < kMax_message_length)
      {
        strcpy(decoded[num_decoded], message);

        new_decoded[num_decoded].sync_score = cand.score;
        new_decoded[num_decoded].freq_hz = (int)freq_hz;
        strcpy(new_decoded[num_decoded].call_to, call_to);
        strcpy(new_decoded[num_decoded].call_from, call_from);
        strcpy(new_decoded[num_decoded].locator, locator);

        new_decoded[num_decoded].slot = slot_state;

        raw_RSL = (float)cand.score;
        display_RSL = (int)((raw_RSL - 235)) / 8;
        new_decoded[num_decoded].snr = display_RSL;
        new_decoded[num_decoded].sequence = Seq_RSL;

        new_decoded[num_decoded].target_distance = 0;

        if (validate_locator(locator))
        {
          strcpy(new_decoded[num_decoded].target_locator, locator);
          new_decoded[num_decoded].sequence = Seq_Locator;
        }
        else
        {
          const char *ptr = locator;
          if (*ptr == 'R')
          {
            ptr++;
          }

          received_RSL = atoi(ptr);
          if (received_RSL < 30) // Prevents a 73 being decoded as a received RSL
          {
            new_decoded[num_decoded].received_snr = received_RSL;
          }
        }

        // ignore hashed callsigns
        if (*call_from != '<')
        {
          uint32_t frequency = (sBand_Data[BandIndex].Frequency * 1000) + new_decoded[num_decoded].freq_hz;
          addReceivedRecord(call_from, frequency, display_RSL);
        }

        if (strindex(new_decoded[num_decoded].call_to, "CQ") >= 0)
        {
          new_decoded[num_decoded].calling_CQ = 1;
        }
        else
        {
          new_decoded[num_decoded].calling_CQ = 0;
        }

        ++num_decoded;
      }
    }
  } // End of big decode loop

  return num_decoded;
}

int validate_locator(const char *QSO_locator)
{
  const char RR73[4] = {'R', 'R', '7', '3'};
  return (IsValidLocator(QSO_locator) &&
          0 != memcmp(QSO_locator, RR73, sizeof(RR73)));
}

int strindex(const char *s, const char *t)
{
  int result = -1;

  for (int i = 0; s[i] != '\0'; i++)
  {
    int k = 0;
    for (int j = i; t[k] != '\0' && s[j] == t[k]; j++, k++)
      ;

    if (k > 0 && t[k] == '\0')
      result = i;
  }
  return result;
}

void set_QSO_Xmit_Freq(int freq)
{
  cursor_freq = freq;
  display_value(870, 559, cursor_freq);

  float cursor_value = (float)freq / FFT_Resolution;
  cursor_line = (uint16_t)(cursor_value - ft8_min_bin);
  display_cursor_line = 2 * cursor_line;
}

void process_selected_Station(int stations_decoded, int TouchIndex)
{
  if (stations_decoded > 0 && TouchIndex <= stations_decoded)
  {
    strcpy(Target_Call, new_decoded[TouchIndex].call_from);
    strcpy(Target_Locator, new_decoded[TouchIndex].target_locator);

    Target_RSL = new_decoded[TouchIndex].snr;
    target_slot = new_decoded[TouchIndex].slot ^ 1; // toggle the slot
    int target_freq = new_decoded[TouchIndex].freq_hz;

    if (QSO_Fix)
      set_QSO_Xmit_Freq(target_freq);
  }

  FT8_Touch_Flag = 0;
}

void display_messages(Decode new_decoded[], int decoded_messages)
{
  clear_rx_region();
  max_sync_score = 0;
  Valid_CQ_Candidate = 0;

  for (int i = 0; i < decoded_messages && i < MAX_RX_ROWS; i++)
  {
    const char *call_to = new_decoded[i].call_to;
    const char *call_from = new_decoded[i].call_from;
    const char *locator = new_decoded[i].locator;

    MsgColor color = White;
    char message[MAX_MSG_LEN];
    snprintf(message, MAX_LINE_LEN, "%s %s %s", call_to, call_from, locator);
    message[MAX_LINE_LEN - 1] = '\0'; // Make sure it fits the display region

    if (new_decoded[i].calling_CQ)
    {
      color = Green;

      if (!check_call_list(i) && !check_log_list(i))
      {
        if (new_decoded[i].sync_score > max_sync_score)
        {
          max_sync_score = new_decoded[i].sync_score;
          max_sync_score_index = i;
          Valid_CQ_Candidate = 1;
        }
      }
    }

    // Addressed me
    if (strncmp(call_to, Station_Call, CALLSIGN_SIZE) == 0)
    {
      color = Red;
    }

    // Mark own TX in yellow (WSJT-X)
    if (was_txing)
    {
      color = Yellow;
    }
    display_line(false, i, Black, color, message);
  }
}

void store_CQ_Call(void)
{

  const char blank[] = "             ";

  for (int i = 0; i < auto_call_limit - 1; i++)
  {
    strcpy(call_list[i].call, blank);
    strcpy(call_list[i].call, call_list[i + 1].call);
  }

  strcpy(call_list[auto_call_limit - 1].call, blank);
  strcpy(call_list[auto_call_limit - 1].call, new_decoded[max_sync_score_index].call_from);
}

void store_logged_CQ_Call(const char *call)
{

  strcpy(auto_logged_list[auto_logged].call, call); // store candidate call so we do not duplicate call later
  auto_logged++;
  display_value(0, 520, auto_logged);
}

void clear_auto_memories(void)
{

  for (int j = 0; j < auto_logged; j++)
  {
    strcpy(call_list[j].call, auto_blank);
    call_list[j].distance = 0.0;
    call_list[j].sync_score = 0;
  }
  auto_logged = 0;
}

void display_line(bool right, int line, MsgColor background, MsgColor textcolor, const char *text)
{
  tft.setFontSize(1, true);
  tft.textColor(lcd_color_map[textcolor], lcd_color_map[background]);
  tft.setCursor(right ? START_X_RIGHT : START_X_LEFT, START_Y + line * LINE_HT);
  tft.write((const uint8_t *)text, strlen(text));
}

void display_call_list_item(int left, int line, MsgColor background, MsgColor textcolor, const char *text)
{
  tft.setFontSize(1, true);
  tft.textColor(lcd_color_map[textcolor], lcd_color_map[background]);
  tft.setCursor(left, START_Y + line * LINE_HT);
  tft.write((const uint8_t *)text, strlen(text));
}

void display_call_list(int number_calls)
{

  for (int i = 0; i < number_calls; i++)
    display_call_list_item(600, i, Black, Yellow, call_list[i].call);
}

void clear_rx_region(void)
{
  for (int i = 0; i < MAX_RX_ROWS; i++)
  {
    display_line(false, i, Black, Black, blank);
  }
}

void clear_qso_region(void)
{
  for (int i = 0; i < MAX_QSO_ROWS; i++)
  {
    display_line(true, i + 1, Black, Black, blank);
  }
}

void display_queued_message(const char *msg)
{
  display_line(true, 0, Black, Black, blank);
  display_line(true, 0, Black, Red, msg);
}

void display_txing_message(const char *msg)
{
  display_line(true, 0, Black, Black, blank);
  display_line(true, 0, Red, White, msg);
}

void display_qso_state(const char *txt)
{
  display_line(true, 1, Black, Black, blank);
  display_line(true, 1, Black, White, txt);
}

char *add_worked_qso(void)
{
  // Handle circular buffer overflow - use modulo for array indexing
  int entry_index = num_qsos % MAX_QSO_ENTRIES;
  num_qsos++;
  return worked_qso_entries[entry_index];
}

bool display_worked_qsos(void)
{
  // Display in pages
  // pi is page index
  static int pi = 0;

  // Determine how many entries to show (max 100)
  int total_entries = num_qsos < MAX_QSO_ENTRIES ? num_qsos : MAX_QSO_ENTRIES;

  if (pi * MAX_QSO_ROWS > total_entries)
  {
    pi = 0;
    return false;
  }

  // Clear the entire log region first
  clear_qso_region();

  // Display the log in reverse order (most recent first)
  for (int ri = 0; ri < MAX_QSO_ROWS && (pi * MAX_QSO_ROWS + ri) < total_entries; ++ri)
  {
    // Calculate the QSO index in reverse chronological order
    int paging_offset = pi * MAX_QSO_ROWS + ri;
    int qso_index = num_qsos - 1 - paging_offset;

    // Get the actual array index using modulo for circular buffer
    int array_index = qso_index % MAX_QSO_ENTRIES;

    display_line(true, ri, Black, Green, worked_qso_entries[array_index]);
  }
  ++pi;
  return true;
}

int check_call_list(int message_index)
{

  int test = 0;

  for (int i = 0; i < auto_call_limit; i++)
  {

    if (strcmp(call_list[i].call, new_decoded[message_index].call_from) == 0)
    {
      test = 1;
    }
  }
  return test;
}

int check_log_list(int message_index)
{

  int test = 0;

  for (int i = 0; i < auto_logged; i++)
  {
    if (strcmp(auto_logged_list[i].call, new_decoded[message_index].call_from) == 0)
    {
      test = 1;
    }
  }
  return test;
}
