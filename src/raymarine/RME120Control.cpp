/******************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  Navico BR24 Radar Plugin
 * Author:   David Register
 *           Dave Cowell
 *           Kees Verruijt
 *           Hakan Svensson
 *           Douwe Fokkema
 *           Sean D'Epagnier
 *	     RM Guy
 ***************************************************************************
 *   Copyright (C) 2010 by David S. Register              bdbcat@yahoo.com *
 *   Copyright (C) 2012-2013 by Dave Cowell                                *
 *   Copyright (C) 2012-2016 by Kees Verruijt         canboat@verruijt.net *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************
 */

#include "RME120Control.h"

PLUGIN_BEGIN_NAMESPACE

RME120Control::RME120Control() {
  m_radar_socket = INVALID_SOCKET;
  m_name = wxT("RME120 radar");
}

RME120Control::~RME120Control() {
  if (m_radar_socket != INVALID_SOCKET) {
    closesocket(m_radar_socket);
    LOG_TRANSMIT(wxT("radar_pi: %s transmit socket closed"), m_name.c_str());
  }
  int i = 0;
  i++;
}

bool RME120Control::Init(radar_pi *pi, RadarInfo *ri, NetworkAddress &ifadr, NetworkAddress &radaradr) {
  int r;
  int one = 1;

  // The radar IP address is not used for Navico BR/Halo radars
  if (radaradr.port != 0) {
    // Null
  }

  m_pi = pi;
  m_ri = ri;
  m_name = ri->m_name;

  if (m_radar_socket != INVALID_SOCKET) {
    closesocket(m_radar_socket);
  }
  m_radar_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (m_radar_socket == INVALID_SOCKET) {
    r = -1;
  } else {
    r = setsockopt(m_radar_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
  }

  if (!r) {
    struct sockaddr_in s = ifadr.GetSockAddrIn();

    r = ::bind(m_radar_socket, (struct sockaddr *)&s, sizeof(s));
  }

  if (r) {
    wxLogError(wxT("radar_pi: Unable to create UDP sending socket"));
    LOG_INFO(wxT("$$$ tx socketerror "));
    // Might as well give up now
    return false;
  }
  return true;
}

void RME120Control::logBinaryData(const wxString &what, const uint8_t *data, int size) {
  wxString explain;
  int i = 0;

  explain.Alloc(size * 3 + 50);
  explain += wxT("radar_pi: ") + m_name.c_str() + wxT(" ");
  explain += what;
  explain += wxString::Format(wxT(" %d bytes: "), size);
  for (i = 0; i < size; i++) {
    explain += wxString::Format(wxT(" %02X"), data[i]);
  }
  LOG_TRANSMIT(explain);
}

bool RME120Control::TransmitCmd(const uint8_t *msg, int size) {
  if (m_radar_socket == INVALID_SOCKET) {
    wxLogError(wxT("radar_pi: Unable to transmit command to unknown radar"));
    return false;
  }
  if (sendto(m_radar_socket, (char *)msg, size, 0, (struct sockaddr *)&m_addr, sizeof(m_addr)) < size) {
    wxLogError(wxT("radar_pi: Unable to transmit command to %s: %s"), m_name.c_str(), SOCKETERRSTR);
    return false;
  }
  IF_LOG_AT(LOGLEVEL_TRANSMIT, logBinaryData(wxT("$$$transmit"), msg, size));
  return true;
}

static uint8_t rd_msg_tx_control[] = {0x01, 0x80, 0x01, 0x00,
                                      0x00,  // Control value at offset 4 : 0 - off, 1 - on
                                      0x00, 0x00, 0x00};

void RME120Control::RadarTxOff() {
  rd_msg_tx_control[4] = 0;
  TransmitCmd(rd_msg_tx_control, sizeof(rd_msg_tx_control));
}

void RME120Control::RadarTxOn() {
  rd_msg_tx_control[4] = 1;
  TransmitCmd(rd_msg_tx_control, sizeof(rd_msg_tx_control));
};

static uint8_t rd_msg_5s[] = {
    0x03, 0x89, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // keeps alive for 5 seconds ?
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x01, 0x00, 0x00,
    0x9e, 0x03, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

bool RME120Control::RadarStayAlive() {
  TransmitCmd(rd_msg_5s, sizeof(rd_msg_5s));
  return true;
}

static uint8_t rd_msg_set_range[] = {0x01, 0x81, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
                                     0x01,  // Range at offset 8 (0 - 1/8, 1 - 1/4, 2 - 1/2, 3 - 3/4, 4 - 1, 5 - 1.5, 6 - 3...)
                                     0x00, 0x00, 0x00};   // length == 12

static int radar_ranges[] = {1852 / 4,  1852 / 2,  1852,      1852 * 3 / 2, 1852 * 3,  1852 * 6,  // update from RadarFacory  ?? $$$
                             1852 * 12, 1852 * 24, 1852 * 48, 1852 * 96,    1852 * 144};
static int current_ranges[11] = {125, 250, 500, 750, 1500, 3000, 6000, 12000, 24000, 48000, 72000};

bool RME120Control::SetRange(int meters) {
  for (int i = 0; i < sizeof(radar_ranges) / sizeof(radar_ranges[0]); i++) {
    if (meters <= radar_ranges[i]) {
      SetRangeIndex(i);
      return true;
    }
  }
  SetRange(sizeof(radar_ranges) / sizeof(radar_ranges[0]) - 1);
  return false;
}

void RME120Control::SetRangeIndex(size_t index) {
  LOG_INFO(wxT("$$$ SetRangeIndex index = %i"), index);
  rd_msg_set_range[8] = index;
  TransmitCmd(rd_msg_set_range, sizeof(rd_msg_set_range));
}

bool RME120Control::SetControlValue(ControlType controlType, RadarControlItem &item, RadarControlButton *button) {
  bool r = false;
  LOG_INFO(wxT("$$$ SetControlValue called"));
  int value = item.GetValue();
  RadarControlState state = item.GetState();
  int autoValue = 0;
  if (state > RCS_MANUAL) {
    autoValue = state - RCS_MANUAL;
  }

  switch (controlType) {
    case CT_NONE:
    case CT_RANGE:
    case CT_TIMED_IDLE:
    case CT_TIMED_RUN:
    case CT_TRANSPARENCY:
    case CT_REFRESHRATE:
    case CT_TARGET_ON_PPI:
    case CT_TARGET_TRAILS:
    case CT_TRAILS_MOTION:
    case CT_MAIN_BANG_SIZE:
    case CT_MAX:
    case CT_ORIENTATION:
    case CT_CENTER_VIEW:
    case CT_OVERLAY_CANVAS:
    case CT_ANTENNA_FORWARD:
    case CT_ANTENNA_STARBOARD:
    case CT_NO_TRANSMIT_START:
    case CT_NO_TRANSMIT_END:
    case CT_FTC:
      // The above are not settings that are not radar commands or not supported by Navico radar.
      // Made them explicit so the compiler can catch missing control types.
      break;

      // Ordering the radar commands by the first byte value.
      // Some interesting holes here, seems there could be more commands!

    case CT_BEARING_ALIGNMENT: {  // to be consistent with the local bearing alignment of the pi
                                  // this bearing alignment works opposite to the one an a Lowrance display
      if (value < 0) {
        value += 360;
      }
      uint8_t cmd[] = {0x07, 0x82, 0x01, 0x00, 0x14, 0x00, 0x00, 0x00};

      cmd[4] = value & 0xff;
      cmd[5] = (value >> 8) & 0xff;
      cmd[6] = (value >> 16) & 0xff;
      cmd[7] = (value >> 24) & 0xff;
      LOG_VERBOSE(wxT("radar_pi: %s $$$Bearing alignment: %d"), m_name.c_str(), value);
      r = TransmitCmd(cmd, sizeof(cmd));
      break;
    }

    case CT_GAIN: {
      uint8_t cmd[] = {0x01, 0x83, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                       0x00,  // Gain value at offset 20
                       0x00, 0x00, 0x00};

      uint8_t cmd2[] = {0x01, 0x83, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x01,  // Gain auto - 1, manual - 0 at offset 16
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

      if (!autoValue) {
         cmd2[16] = 0;
         r = TransmitCmd(cmd2, sizeof(cmd2));  // set auto off
        cmd[20] = value;
        r = TransmitCmd(cmd, sizeof(cmd));        
      } else {                                // auto on
        cmd2[16] = 1;
        r = TransmitCmd(cmd2, sizeof(cmd2));  // set auto on
      }
      LOG_VERBOSE(wxT("radar_pi: %s Gain: %d auto %d"), m_name.c_str(), value, autoValue);
      break;
    }

    //case CT_SEA: {
    //  int v = (value + 1) * 255 / 100;
    //  if (v > 255) {
    //    v = 255;
    //  }
    //  uint8_t cmd[] = {0x06, 0xc1, 0x02, 0, 0, 0, (uint8_t)autoValue, 0, 0, 0, (uint8_t)v};
    //  LOG_VERBOSE(wxT("radar_pi: %s Sea: %d auto %d"), m_name.c_str(), value, autoValue);
    //  r = TransmitCmd(cmd, sizeof(cmd));
    //  break;
    //}

    //case CT_RAIN: {  // Rain Clutter - Manual. Range is 0x01 to 0x50
    //  int v = (value + 1) * 255 / 100;
    //  if (v > 255) {
    //    v = 255;
    //  }
    //  uint8_t cmd[] = {0x06, 0xc1, 0x04, 0, 0, 0, 0, 0, 0, 0, (uint8_t)v};
    //  LOG_VERBOSE(wxT("radar_pi: %s Rain: %d"), m_name.c_str(), value);
    //  r = TransmitCmd(cmd, sizeof(cmd));
    //  break;
    //}

    //case CT_SIDE_LOBE_SUPPRESSION: {
    //  int v = value * 256 / 100;
    //  if (v > 255) {
    //    v = 255;
    //  }
    //  uint8_t cmd[] = {0x6, 0xc1, 0x05, 0, 0, 0, (uint8_t)autoValue, 0, 0, 0, (uint8_t)v};
    //  LOG_VERBOSE(wxT("radar_pi: %s command Tx CT_SIDE_LOBE_SUPPRESSION: %d auto %d"), m_name.c_str(), value, autoValue);
    //  r = TransmitCmd(cmd, sizeof(cmd));
    //  break;
    //}

    //  // What would command 7 be?

    //case CT_INTERFERENCE_REJECTION: {
    //  uint8_t cmd[] = {0x08, 0xc1, (uint8_t)value};
    //  LOG_VERBOSE(wxT("radar_pi: %s Rejection: %d"), m_name.c_str(), value);
    //  r = TransmitCmd(cmd, sizeof(cmd));
    //  break;
    //}

    //case CT_TARGET_EXPANSION: {
    //  uint8_t cmd[] = {0x09, 0xc1, (uint8_t)value};
    //  LOG_VERBOSE(wxT("radar_pi: %s Target expansion: %d"), m_name.c_str(), value);
    //  r = TransmitCmd(cmd, sizeof(cmd));
    //  break;
    //}

    //case CT_TARGET_BOOST: {
    //  uint8_t cmd[] = {0x0a, 0xc1, (uint8_t)value};
    //  LOG_VERBOSE(wxT("radar_pi: %s Target boost: %d"), m_name.c_str(), value);
    //  r = TransmitCmd(cmd, sizeof(cmd));
    //  break;
    //}

    //  // What would command b through d be?

    //case CT_LOCAL_INTERFERENCE_REJECTION: {
    //  if (value < 0) value = 0;
    //  if (value > 3) value = 3;
    //  uint8_t cmd[] = {0x0e, 0xc1, (uint8_t)value};
    //  LOG_VERBOSE(wxT("radar_pi: %s local interference rejection %d"), m_name.c_str(), value);
    //  r = TransmitCmd(cmd, sizeof(cmd));
    //  break;
    //}

    //case CT_SCAN_SPEED: {
    //  uint8_t cmd[] = {0x0f, 0xc1, (uint8_t)value};
    //  LOG_VERBOSE(wxT("radar_pi: %s Scan speed: %d"), m_name.c_str(), value);
    //  r = TransmitCmd(cmd, sizeof(cmd));
    //  break;
    //}

    //  // What would command 10 through 20 be?

    //case CT_NOISE_REJECTION: {
    //  uint8_t cmd[] = {0x21, 0xc1, (uint8_t)value};
    //  LOG_VERBOSE(wxT("radar_pi: %s Noise rejection: %d"), m_name.c_str(), value);
    //  r = TransmitCmd(cmd, sizeof(cmd));
    //  break;
    //}

    //case CT_TARGET_SEPARATION: {
    //  uint8_t cmd[] = {0x22, 0xc1, (uint8_t)value};
    //  LOG_VERBOSE(wxT("radar_pi: %s Target separation: %d"), m_name.c_str(), value);
    //  r = TransmitCmd(cmd, sizeof(cmd));
    //  break;
    //}

    //case CT_DOPPLER: {
    //  uint8_t cmd[] = {0x23, 0xc1, (uint8_t)value};
    //  LOG_VERBOSE(wxT("radar_pi: %s Doppler state: %d"), m_name.c_str(), value);
    //  r = TransmitCmd(cmd, sizeof(cmd));
    //  break;
    //}

    //  // What would command 24 through 2f be?

    //case CT_ANTENNA_HEIGHT: {
    //  int v = value * 1000;  // radar wants millimeters, not meters :-)
    //  int v1 = v / 256;
    //  int v2 = v & 255;
    //  uint8_t cmd[10] = {0x30, 0xc1, 0x01, 0, 0, 0, (uint8_t)v2, (uint8_t)v1, 0, 0};
    //  LOG_VERBOSE(wxT("radar_pi: %s Antenna height: %d"), m_name.c_str(), v);
    //  r = TransmitCmd(cmd, sizeof(cmd));
    //  break;
    //}
  }

  return r;
}

PLUGIN_END_NAMESPACE
