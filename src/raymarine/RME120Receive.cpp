/******************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  Radar Plugin
 * Author:   David Register
 *           Dave Cowell
 *           Kees Verruijt
 *           Hakan Svensson
 *           Douwe Fokkema
 *           Sean D'Epagnier
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

#include "MessageBox.h"
#include "RME120Control.h"
#include "RME120Receive.h"

PLUGIN_BEGIN_NAMESPACE

/*
 * This file not only contains the radar receive threads, it is also
 * the only unit that understands what the radar returned data looks like.
 * The rest of the plugin uses a (slightly) abstract definition of the radar.
 */

#define MILLIS_PER_SELECT 250
#define SECONDS_SELECT(x) ((x)*MILLISECONDS_PER_SECOND / MILLIS_PER_SELECT)
#define MOD_ROTATION2048(raw) (((raw) + 2 * LINES_PER_ROTATION) % LINES_PER_ROTATION)
#define LINES_PER_ROTATION (2048)  // but use only half that in practice
#define SCALE_DEGREES_TO_RAW(angle) ((int)((angle) * (double)SPOKES / DEGREES_PER_ROTATION))

//
// Raymarine radars use an internal spoke ID that has range [0..4096> but they
// only send half of them
//
#define SPOKES (4096)
#define SCALE_RAW_TO_DEGREES(raw) ((raw) * (double)DEGREES_PER_ROTATION / SPOKES)
#define SCALE_DEGREES_TO_RAW(angle) ((int)((angle) * (double)SPOKES / DEGREES_PER_ROTATION))


#define HEADING_TRUE_FLAG 0x4000
#define HEADING_MASK (SPOKES - 1)
#define HEADING_VALID(x) (((x) & ~(HEADING_TRUE_FLAG | HEADING_MASK)) == 0)


SOCKET RME120Receive::PickNextEthernetCard() {
  SOCKET socket = INVALID_SOCKET;
  CLEAR_STRUCT(m_interface_addr);

  // Pick the next ethernet card
  // If set, we used this one last time. Go to the next card.
  if (m_interface) {
    m_interface = m_interface->ifa_next;
  }
  // Loop until card with a valid IPv4 address
  while (m_interface && !VALID_IPV4_ADDRESS(m_interface)) {
    m_interface = m_interface->ifa_next;
  }
  if (!m_interface) {
    if (m_interface_array) {
      freeifaddrs(m_interface_array);
      m_interface_array = 0;
    }
    if (!getifaddrs(&m_interface_array)) {
      m_interface = m_interface_array;
    }
    // Loop until card with a valid IPv4 address
    while (m_interface && !VALID_IPV4_ADDRESS(m_interface)) {
      m_interface = m_interface->ifa_next;
    }
  }
  if (m_interface && VALID_IPV4_ADDRESS(m_interface)) {
    m_interface_addr.addr = ((struct sockaddr_in *)m_interface->ifa_addr)->sin_addr;
    m_interface_addr.port = 0;
  }
  socket = GetNewReportSocket();
  return socket;
}

SOCKET RME120Receive::GetNewReportSocket() {
  SOCKET socket;
  wxString error = wxT("");
  wxString s = wxT("");

  if (!(m_info == m_pi->GetNavicoRadarInfo(m_ri->m_radar))) {  // initial values or NavicoLocate modified the info
    m_info = m_pi->GetNavicoRadarInfo(m_ri->m_radar);
    m_interface_addr = m_pi->GetRadarInterfaceAddress(m_ri->m_radar);
    UpdateSendCommand();
    LOG_INFO(wxT("radar_pi: %s Locator found radar at IP %s [%s]"), m_ri->m_name,
             M_SETTINGS.radar_address[m_ri->m_radar].FormatNetworkAddressPort(), m_info.to_string());
  };

  if (m_interface_addr.IsNull() || m_info.report_addr.IsNull()) {
    LOG_RECEIVE(wxT("radar_pi: %s no address to listen on"), m_ri->m_name);
    return INVALID_SOCKET;
  }

  if (RadarOrder[m_ri->m_radar_type] >= RO_PRIMARY) {
    if (!m_info.serialNr.IsNull()) {
      s << _("Serial #") << m_info.serialNr << wxT("\n");
    }
  }

  socket = startUDPMulticastReceiveSocket(m_interface_addr, m_info.report_addr, error);

  if (socket != INVALID_SOCKET) {
    wxString addr = m_interface_addr.FormatNetworkAddress();
    wxString rep_addr = m_info.report_addr.FormatNetworkAddressPort();

    LOG_RECEIVE(wxT("radar_pi: %s scanning interface %s for data from %s"), m_ri->m_name, addr.c_str(), rep_addr.c_str());

    s << _("Scanning interface") << wxT(" ") << addr;
    SetInfoStatus(s);
  } else {
    s << error;
    SetInfoStatus(s);
    wxLogError(wxT("radar_pi: %s Unable to listen to socket: %s"), m_ri->m_name, error.c_str());
  }
  return socket;
}


/*
 * Entry
 *
 * Called by wxThread when the new thread is running.
 * It should remain running until Shutdown is called.
 */
void *RME120Receive::Entry(void) {
  int r = 0;
  int no_data_timeout = 0;
  int no_spoke_timeout = 0;
  union {
    sockaddr_storage addr;
    sockaddr_in ipv4;
  } rx_addr;
  socklen_t rx_len;

  uint8_t data[2048];  // largest packet seen so far from a Raymarine is 626
  m_interface_array = 0;
  m_interface = 0;
  struct sockaddr_in radarFoundAddr;
  sockaddr_in *radar_addr = 0;

  SOCKET reportSocket = INVALID_SOCKET;
  UpdateSendCommand();  // $$$ may be not needed, but does not seem to harm
  LOG_VERBOSE(wxT("radar_pi: RamarineReceive thread %s starting"), m_ri->m_name.c_str());
  reportSocket = GetNewReportSocket();  // Start using the same interface_addr as previous time

  while (m_receive_socket != INVALID_SOCKET) {
    if (reportSocket == INVALID_SOCKET) {
      reportSocket = PickNextEthernetCard();
      if (reportSocket != INVALID_SOCKET) {
        no_data_timeout = 0;
        no_spoke_timeout = 0;
      }
    }

    struct timeval tv = {(long)0, (long)(MILLIS_PER_SELECT * 1000)};

    fd_set fdin;
    FD_ZERO(&fdin);

    int maxFd = INVALID_SOCKET;
    if (m_receive_socket != INVALID_SOCKET) {
      FD_SET(m_receive_socket, &fdin);
      maxFd = MAX(m_receive_socket, maxFd);
    }
    if (reportSocket != INVALID_SOCKET) {
      FD_SET(reportSocket, &fdin);
      maxFd = MAX(reportSocket, maxFd);
    }
   
    wxLongLong start = wxGetUTCTimeMillis();
    r = select(maxFd + 1, &fdin, 0, 0, &tv);
    LOG_RECEIVE(wxT("radar_pi: select maxFd=%d r=%d elapsed=%lld"), maxFd, r, wxGetUTCTimeMillis() - start);

    if (r > 0) {
      if (m_receive_socket != INVALID_SOCKET && FD_ISSET(m_receive_socket, &fdin)) {
        rx_len = sizeof(rx_addr);
        r = recvfrom(m_receive_socket, (char *)data, sizeof(data), 0, (struct sockaddr *)&rx_addr, &rx_len);
        if (r > 0) {
          LOG_VERBOSE(wxT("radar_pi: %s received stop instruction"), m_ri->m_name.c_str());
          break;
        }
      }

      if (reportSocket != INVALID_SOCKET && FD_ISSET(reportSocket, &fdin)) {
        rx_len = sizeof(rx_addr);
        r = recvfrom(reportSocket, (char *)data, sizeof(data), 0, (struct sockaddr *)&rx_addr, &rx_len);
        if (r > 0) {
          NetworkAddress radar_address;
          radar_address.addr = rx_addr.ipv4.sin_addr;
          radar_address.port = rx_addr.ipv4.sin_port;

          ProcessFrame(data, (size_t)r);
            if (!radar_addr) {
              wxCriticalSectionLocker lock(m_lock);
              m_ri->DetectedRadar(m_interface_addr, radar_address);  // enables transmit data
              UpdateSendCommand();
      
              radarFoundAddr = rx_addr.ipv4;
              radar_addr = &radarFoundAddr;

              if (m_ri->m_state.GetValue() == RADAR_OFF) {
                LOG_INFO(wxT("radar_pi: %s detected at %s"), m_ri->m_name.c_str(), radar_address.FormatNetworkAddress());
                m_ri->m_state.Update(RADAR_STANDBY);
              }
            }
            no_data_timeout = SECONDS_SELECT(-15);
          
        }
      }

    } else {  // no data received -> select timeout
      if (no_data_timeout >= SECONDS_SELECT(2)) {
        no_data_timeout = 0;
        if (reportSocket != INVALID_SOCKET) {
          closesocket(reportSocket);
          reportSocket = INVALID_SOCKET;
          m_ri->m_state.Update(RADAR_OFF);
          CLEAR_STRUCT(m_interface_addr);
          radar_addr = 0;
        }
      } else {
        no_data_timeout++;
      }

      if (no_spoke_timeout >= SECONDS_SELECT(2)) {
        no_spoke_timeout = 0;
        m_ri->ResetRadarImage();
      } else {
        no_spoke_timeout++;
      }
    }

    if (!(m_info == m_pi->GetNavicoRadarInfo(m_ri->m_radar))) {
      // Navicolocate modified the RadarInfo in settings
      closesocket(reportSocket);
      reportSocket = INVALID_SOCKET;
    };

    if (reportSocket == INVALID_SOCKET) {
      // If we closed the reportSocket then close the command socket
    }

  }  // endless loop until thread destroy

  if (reportSocket != INVALID_SOCKET) {
    closesocket(reportSocket);
  }
  if (m_send_socket != INVALID_SOCKET) {
    closesocket(m_send_socket);
    m_send_socket = INVALID_SOCKET;
  }
  if (m_receive_socket != INVALID_SOCKET) {
    closesocket(m_receive_socket);
  }

  if (m_interface_array) {
    freeifaddrs(m_interface_array);
  }

#ifdef TEST_THREAD_RACES
  LOG_VERBOSE(wxT("radar_pi: %s receive thread sleeping"), m_ri->m_name.c_str());
  wxMilliSleep(1000);
#endif
  m_is_shutdown = true;
  return 0;
}

//void RME120Receive::SetRadarType(RadarType t) {  // $$$
//  m_ri->m_radar_type = t;
//  // m_pi->m_pMessageBox->SetRadarType(t);
//}



void RME120Receive::ProcessFrame(const UINT8 *data, size_t len) {   // This is the original ProcessFrame from RMradar_pi
  wxLongLong nowMillis = wxGetLocalTimeMillis();
  time_t now = time(0);
  m_ri->resetTimeout(now);

  /*if (m_quit || !m_pi->m_initialized) {
    return;
  }*/

  m_ri->m_radar_timeout = now + WATCHDOG_TIMEOUT;

  int spoke = 0;
  m_ri->m_statistics.packets++;

  if (len >= 4) {
    uint32_t msgId = 0;
    memcpy(&msgId, data, sizeof(msgId));
    switch (msgId) {
      case 0x00010001:
        ProcessFeedback(data, len);
        break;
      case 0x00010002:
        ProcessPresetFeedback(data, len);
        break;
      case 0x00010003:
        ProcessScanData(data, len);
        m_ri->m_data_timeout = now + DATA_TIMEOUT;
        break;
      case 0x00010005:
        ProcessCurveFeedback(data, len);
        break;
      case 0x00010006:
      case 0x00010007:
      case 0x00010008:
      case 0x00010009:
      case 0x00018942:
        break;
      default:
        // fprintf(stderr, "Unknown message ID %08X.\n", (int)msgId);
        break;
    }
  }
}

void RME120Receive::UpdateSendCommand() {
  if (!m_info.send_command_addr.IsNull() && m_ri->m_control) {  // $$$ to be tested
    RadarControl *control = (RadarControl *)m_ri->m_control;
    control->SetMultiCastAddress(m_info.send_command_addr);
  }
}

#pragma pack(push, 1)

struct SRadarFeedback {
  uint32_t type;  // 0x010001
  uint32_t range_values[11];
  uint32_t something_1[33];
  uint8_t status;  // 2 - warmup, 1 - transmit, 0 - standby, 6 - shutting down (warmup time - countdown), 3 - shutdown
  uint8_t something_2[3];
  uint8_t warmup_time;
  uint8_t signal_strength;  // number of bars
  uint8_t something_3[7];
  uint8_t range_id;
  uint8_t something_4[2];
  uint8_t auto_gain;
  uint8_t something_5[3];
  uint32_t gain;
  uint8_t auto_sea;  // 0 - disabled; 1 - harbour, 2 - offshore, 3 - coastal
  uint8_t something_6[3];
  uint8_t sea_value;
  uint8_t rain_enabled;
  uint8_t something_7[3];
  uint8_t rain_value;
  uint8_t ftc_enabled;
  uint8_t something_8[3];
  uint8_t ftc_value;
  uint8_t auto_tune;
  uint8_t something_9[3];
  uint8_t tune;
  int16_t bearing_offset;  // degrees * 10; left - negative, right - positive
  uint8_t interference_rejection;
  uint8_t something_10[3];
  uint8_t target_expansion;
  uint8_t something_11[13];
  uint8_t mbs_enabled;  // Main Bang Suppression enabled if 1
};

struct SRadarPresetFeedback {
  uint32_t type;             // 0x010002
  uint8_t something_1[213];  // 221 - magnetron current; 233, 234 - rotation time ms (251 total)
  uint16_t magnetron_hours;
  uint8_t something_2[6];
  uint8_t magnetron_current;
  uint8_t something_3[11];
  uint16_t rotation_time;
  uint8_t something_4[13];
  uint8_t stc_preset_max;
  uint8_t something_5[2];
  uint8_t coarse_tune_arr[3];
  uint8_t fine_tune_arr[3];  // 0, 1, 2 - fine tune value for SP, MP, LP
  uint8_t something_6[6];
  uint8_t display_timing_value;
  uint8_t something_7[12];
  uint8_t stc_preset_value;
  uint8_t something_8[12];
  uint8_t min_gain;
  uint8_t max_gain;
  uint8_t min_sea;
  uint8_t max_sea;
  uint8_t min_rain;
  uint8_t max_rain;
  uint8_t min_ftc;
  uint8_t max_ftc;
  uint8_t gain_value;
  uint8_t sea_value;
  uint8_t fine_tune_value;
  uint8_t coarse_tune_value;
  uint8_t signal_strength_value;
  uint8_t something_9[2];
};

struct SCurveFeedback {
  uint32_t type;  // 0x010005
  uint8_t curve_value;
};

#pragma pack(pop)

static uint8_t radar_signature_id[4] = {1, 0, 0, 0};
static char *radar_signature = (char *)"Ethernet Dome";
struct SRMRadarFunc {
  uint32_t type;
  uint32_t dev_id;
  uint32_t func_id;  // 1
  uint32_t something_1;
  uint32_t something_2;
  uint32_t mcast_ip;
  uint32_t mcast_port;
  uint32_t radar_ip;
  uint32_t radar_port;
};	

static int radar_ranges[] = {1852 / 4,  1852 / 2,  1852,      1852 * 3 / 2, 1852 * 3,  1852 * 6,
                             1852 * 12, 1852 * 24, 1852 * 48, 1852 * 96,    1852 * 144};
static int current_ranges[11] = {125, 250, 500, 750, 1500, 3000, 6000, 12000, 24000, 48000, 72000};

void RME120Receive::ProcessFeedback(const UINT8 *data, int len) {
  if (len == sizeof(SRadarFeedback)) {
    SRadarFeedback *fbPtr = (SRadarFeedback *)data;
    if (fbPtr->type == 0x010001) {
      switch (fbPtr->status) {
        case 0:
          wxLogMessage(wxT("RMRadar_pi: %s received transmit off from %s"), m_ri->m_name.c_str(), "--" /*addr.c_str()*/);
          m_ri->m_state.Update(RADAR_STANDBY);
          break;
        case 1:
          wxLogMessage(wxT("RMRadar_pi: %s received transmit on from %s"), m_ri->m_name.c_str(), "--" /*addr.c_str()*/);
          m_ri->m_state.Update(RADAR_TRANSMIT);
          break;
        case 2:  // Warmup
          wxLogMessage(wxT("RMRadar_pi: %s radar is warming up %s"), m_ri->m_name.c_str(), "--" /*addr.c_str()*/);
          m_ri->m_state.Update(RADAR_STARTING);
          break;
        case 3:  // Off
          wxLogMessage(wxT("RMRadar_pi: %s radar is off %s"), m_ri->m_name.c_str(), "--" /*addr.c_str()*/);
          m_ri->m_state.Update(RADAR_OFF);
          break;
        default:
          m_ri->m_state.Update(RADAR_STANDBY);
          break;
      }
      if (fbPtr->range_values[0] != current_ranges[0])  // Units must have changed
      {
        for (int i = 0; i < sizeof(current_ranges) / sizeof(current_ranges[0]); i++) {
          current_ranges[i] = fbPtr->range_values[i];
          radar_ranges[i] = 1852 * fbPtr->range_values[i] / 500;

          wxLogMessage(wxT("$$$radar ranges %d (%d)\n"), current_ranges[i], radar_ranges[i]);  //  get ranges from type/h
        }
      }
      if (radar_ranges[fbPtr->range_id] != m_range_meters) {
        // if (m_pi->m_settings.verbose >= 1)
        {
          wxLogMessage(wxT("RMRadar_pi: %s now scanning with range %d meters (was %d meters)"), m_ri->m_name.c_str(),
                       radar_ranges[fbPtr->range_id], m_range_meters);
        }
        m_range_meters = radar_ranges[fbPtr->range_id];
        m_updated_range = true;
        m_ri->m_range.Update(m_range_meters / 2);  // RM MFD shows half of what is received
      }


      RadarControlState state;
      state = (fbPtr->auto_gain > 0) ? RCS_AUTO_1 : RCS_MANUAL;
      m_ri->m_gain.Update(fbPtr->gain, state);
            
      state = (fbPtr->auto_sea > 0) ? RCS_AUTO_1 : RCS_MANUAL;
      state = (RadarControlState) fbPtr->auto_sea;
      m_ri->m_sea.UpdateState(state);
     
      m_ri->m_rain.Update(fbPtr->rain_value);

      m_ri->m_target_expansion.Update(fbPtr->target_expansion);
      m_ri->m_interference_rejection.Update(fbPtr->interference_rejection);

      int ba = (int)fbPtr->bearing_offset;
      m_ri->m_bearing_alignment.Update(ba);

     state = (fbPtr->auto_tune > 0) ? RCS_AUTO_1 : RCS_MANUAL;
      m_ri->m_tune_fine.Update(fbPtr->tune, state);

      state = (fbPtr->auto_tune > 0) ? RCS_AUTO_1 : RCS_MANUAL;
      m_ri->m_tune_coarse.UpdateState(state);

      m_ri->m_main_bang_suppression.Update(fbPtr->mbs_enabled);

      m_ri->m_warmup_time.Update(fbPtr->warmup_time);
      m_ri->m_signal_strength.Update(fbPtr->signal_strength);

      m_ri->m_ftc.Update(fbPtr->ftc_value);

      /*if (m_ri->m_control_dialog) {
        wxCommandEvent event(wxEVT_COMMAND_TEXT_UPDATED, ID_CONTROL_DIALOG_REFRESH);
        m_ri->m_control_dialog->GetEventHandler()->AddPendingEvent(event);
      }*/
    }
  }
}

void RME120Receive::ProcessPresetFeedback(const UINT8 *data, int len) {
  if (len == sizeof(SRadarPresetFeedback)) {
    SRadarPresetFeedback *fbPtr = (SRadarPresetFeedback *)data;

    // In this system max en min values are fixed

   /* m_tuneCoarse.Update(fbPtr->coarse_tune_value);
    m_stc.Update(fbPtr->stc_preset_value);
    m_displayTiming.Update(fbPtr->display_timing_value);
    m_stc.UpdateMax(fbPtr->stc_preset_max);
    m_gain.SetMin(fbPtr->min_gain);
    m_gain.SetMax(fbPtr->max_gain);
    m_sea.SetMin(fbPtr->min_sea);
    m_sea.SetMax(fbPtr->max_sea);
    m_rain.SetMin(fbPtr->min_rain);
    m_rain.SetMax(fbPtr->max_rain);
    m_ftc.SetMin(fbPtr->min_ftc);
    m_ftc.SetMax(fbPtr->max_ftc);

    m_miscInfo.m_signalStrength = fbPtr->signal_strength_value;
    m_miscInfo.m_magnetronCurrent = fbPtr->magnetron_current;
    m_miscInfo.m_magnetronHours = fbPtr->magnetron_hours;
    m_miscInfo.m_rotationPeriod = fbPtr->rotation_time;*/
  }
}

void RME120Receive::ProcessCurveFeedback(const UINT8 *data, int len) {
  if (len == sizeof(SCurveFeedback)) {
    SCurveFeedback *fbPtr = (SCurveFeedback *)data;

    // better replace this with a translatio table
    switch (fbPtr->curve_value) {
      case 0:
        m_ri->m_stc_curve.Update(1);
        break;
      case 1:
        m_ri->m_stc_curve.Update(2);
        break;
      case 2:
        m_ri->m_stc_curve.Update(3);
        break;
      case 4:
        m_ri->m_stc_curve.Update(4);
        break;
      case 6:
        m_ri->m_stc_curve.Update(5);
        break;
      case 8:
        m_ri->m_stc_curve.Update(6);
        break;
      case 10:
        m_ri->m_stc_curve.Update(7);
        break;
      case 13:
        m_ri->m_stc_curve.Update(8);
        break;
      default:
        fprintf(stderr, "ProcessCurveFeedback: unknown curve value %d.\n", (int)fbPtr->curve_value);
    }
  } else {
    fprintf(stderr, "ProcessCurveFeedback: got %d bytes, expected %d.\n", len, (int)sizeof(SCurveFeedback));
  }
}

// Radar data

struct CRMPacketHeader {
  uint32_t type;  // 0x00010003
  uint32_t zero_1;
  uint32_t something_1;  // 0x0000001c
  uint32_t nspokes;      // 0x00000008 - usually but changes
  uint32_t spoke_count;  // 0x00000000 in regular, counting in HD
  uint32_t zero_3;
  uint32_t something_3;  // 0x00000001
  uint32_t something_4;  // 0x00000000 or 0xffffffff in regular, 0x400 in HD
};

struct CRMRecordHeader {
  uint32_t type;
  uint32_t length;
  // ...
};

struct CRMScanHeader {
  uint32_t type;    // 0x00000001
  uint32_t length;  // 0x00000028
  uint32_t azimuth;
  uint32_t something_2;  // 0x00000001 - 0x03 - HD
  uint32_t something_3;  // 0x00000002
  uint32_t something_4;  // 0x00000001 - 0x03 - HD
  uint32_t something_5;  // 0x00000001 - 0x00 - HD
  uint32_t something_6;  // 0x000001f4 - 0x00 - HD
  uint32_t zero_1;
  uint32_t something_7;  // 0x00000001
};

struct CRMOptHeader {  // No idea what is in there
  uint32_t type;       // 0x00000002
  uint32_t length;     // 0x0000001c
  uint32_t zero_2[5];
};

struct CRMScanData {
  uint32_t type;  // 0x00000003
  uint32_t length;
  uint32_t data_len;
  // unsigned char data[rec_len - 8];
};



void RME120Receive::ProcessScanData(const UINT8 *data, int len) {
  if (len > sizeof(CRMPacketHeader) + sizeof(CRMScanHeader)) {
    CRMPacketHeader *pHeader = (CRMPacketHeader *)data;
    RadarType strange_radar_type = RM_E120;
    if (pHeader->type != 0x00010003 || pHeader->something_1 != 0x0000001c || pHeader->something_3 != 0x0000001) {
      fprintf(stderr, "ProcessScanData::Packet header mismatch %x, %x, %x, %x.\n", pHeader->type, pHeader->something_1,
              pHeader->nspokes, pHeader->something_3);
      return;
    }
    m_ri->m_state.Update(RADAR_TRANSMIT);

    if (pHeader->something_4 == 0x400) {
      if (strange_radar_type != RT_4GA) {
        strange_radar_type = RT_4GA;
        LOG_RECEIVE(wxT(" radartype RT_4GA set ???"));
        //m_pi->m_pMessageBox->SetRadarType(RT_4GA);
      }
    } else {
      if (strange_radar_type != RM_E120) {   // RT_BR24 before
        LOG_RECEIVE(wxT(" radartype3"));
        strange_radar_type = RM_E120;
        //m_pi->m_pMessageBox->SetRadarType(RT_BR24);
      }
    }

    wxLongLong nowMillis = wxGetLocalTimeMillis();
    int headerIdx = 0;
    int nextOffset = sizeof(CRMPacketHeader);

    while (nextOffset < len) {
      CRMScanHeader *sHeader = (CRMScanHeader *)(data + nextOffset);
      if (sHeader->type != 0x00000001 || sHeader->length != 0x00000028) {
        LOG_RECEIVE(wxT("ProcessScanData::Scan header #%d (%d) - %x, %x.\n"), headerIdx, nextOffset, sHeader->type,
                    sHeader->length);
        break;
      }

      if (sHeader->something_2 != 0x00000001 || sHeader->something_3 != 0x00000002 || sHeader->something_4 != 0x00000001 ||
          sHeader->something_5 != 0x00000001 || sHeader->something_6 != 0x000001f4 || sHeader->something_7 != 0x00000001) {
        if (sHeader->something_2 != 3 || sHeader->something_3 != 2 || sHeader->something_4 != 3 || sHeader->something_5 != 0 ||
            sHeader->something_6 != 0 || sHeader->something_7 != 1) {
            LOG_RECEIVE(wxT("ProcessScanData::Scan header #%d part 2 check failed.\n"), headerIdx);
          break;
        } else if (strange_radar_type != RT_4GA) {
          strange_radar_type = RT_4GA;         
          LOG_RECEIVE(wxT("ProcessScanData::Scan header #%d HD second header with regular first.\n"), headerIdx);
        }

      } else if (strange_radar_type != RM_E120) {
        strange_radar_type = RM_E120;
        LOG_RECEIVE(wxT("ProcessScanData::Scan header #%d regular second header with HD first.\n"), headerIdx);
      }

      nextOffset += sizeof(CRMScanHeader);

      CRMRecordHeader *nHeader = (CRMRecordHeader *)(data + nextOffset);
      if (nHeader->type == 0x00000002) {
        if (nHeader->length != 0x0000001c) {
          LOG_RECEIVE(wxT("ProcessScanData::Opt header #%d part 2 check failed.\n"), headerIdx);
        }
        nextOffset += nHeader->length;
      }

      CRMScanData *pSData = (CRMScanData *)(data + nextOffset);

      if ((pSData->type & 0x7fffffff) != 0x00000003 || pSData->length < pSData->data_len + 8) {
        LOG_RECEIVE(wxT("ProcessScanData::Scan data header #%d check failed %x, %d, %d.\n"), headerIdx, pSData->type, pSData->length, pSData->data_len);
        break;
      }
      UINT8 unpacked_data[1024], *dataPtr = 0;
      if (strange_radar_type == RM_E120) {
        uint8_t *dData = (uint8_t *)unpacked_data;
        uint8_t *sData = (uint8_t *)data + nextOffset + sizeof(CRMScanData);

        int iS = 0;
        int iD = 0;
        while (iS < (int) pSData->data_len) {
          if (*sData != 0x5c) {
            *dData++ = (((*sData) & 0x0f) << 4) + 0x0f;
            *dData++ = ((*sData) & 0xf0) + 0x0f;
            sData++;
            iS++;
            iD += 2;
          } else {
            uint8_t nFill = sData[1];
            uint8_t cFill = sData[2];

            for (int i = 0; i < nFill; i++) {
              *dData++ = ((cFill & 0x0f) << 4) + 0x0f;
              *dData++ = (cFill & 0xf0) + 0x0f;
            }
            sData += 3;
            iS += 3;
            iD += nFill * 2;
          }
        }
        if (iD != 512) {
          while (iS < (int)pSData->length - 8 && iD < 512) {
            *dData++ = ((*sData) & 0x0f) << 4;
            *dData++ = (*sData) & 0xf0;
            sData++;
            iS++;
            iD += 2;
          }
        }
        if (iD != 512) {
         /* LOG_INFO(wxT("ProcessScanData::Packet %d line %d (%d/%x) not complete %d.\n"), packetIdx, headerIdx,
           	scan_idx, scan_idx, iD);*/
        }
        dataPtr = unpacked_data;
      } else if (strange_radar_type == RT_4GA) {   // 
        if (pSData->data_len != RETURNS_PER_LINE * 2) {
          m_ri->m_statistics.broken_spokes++;
          LOG_INFO(wxT("ProcessScanData data len %d should be %d.\n"), pSData->data_len, RETURNS_PER_LINE);
          break;
        }
        if (m_range_meters == 0) m_range_meters = 1852 / 4;  // !!!TEMP delete!!!
        dataPtr = (UINT8 *)data + nextOffset + sizeof(CRMScanData);
      } else {
        LOG_INFO(wxT("ProcessScanData::Packet radar type is not set somehow.\n"));
        break;
      }
      nextOffset += pSData->length;
      m_ri->m_statistics.spokes++;
      unsigned int spoke = sHeader->azimuth;
      if (m_next_spoke >= 0 && spoke != m_next_spoke) {
        if ((int)spoke > m_next_spoke) {
          m_ri->m_statistics.missing_spokes += spoke - m_next_spoke;
        } else {
          m_ri->m_statistics.missing_spokes += SPOKES + spoke - m_next_spoke;
        }
      }
      m_next_spoke = (spoke + 1) % 2048;

      if ((pSData->type & 0x80000000) != 0 && nextOffset < len) {
        // fprintf(stderr, "ProcessScanData::Last record %d (%d) in packet %d but still data to go %d:%d.\n",
        // 	headerIdx, scan_idx, packetIdx, nextOffset, len);
      }
      headerIdx++;

      m_pi->SetRadarHeading();

      int hdt_raw = SCALE_DEGREES_TO_RAW(m_pi->GetHeadingTrue() + m_ri->m_viewpoint_rotation);

      int angle_raw = spoke * 2 + SCALE_DEGREES_TO_RAW(180);  // Compensate openGL rotation compared to North UP
      int bearing_raw = angle_raw + hdt_raw;

      SpokeBearing a = MOD_ROTATION2048(angle_raw / 2);    // divide by 2 to map on 2048 scanlines
      SpokeBearing b = MOD_ROTATION2048(bearing_raw / 2);  // divide by 2 to map on 2048 scanlines
      m_ri->ProcessRadarSpoke(a, b, dataPtr, RETURNS_PER_LINE, m_range_meters, nowMillis);
    }
  }
}

static uint8_t rd_msg_1s[] = {0x00, 0x80, 0x01, 0x00, 0x52, 0x41, 0x44, 0x41, 0x52, 0x00, 0x00, 0x00};

//void RME120Receive::Send1sKeepalive() {  // to do $$$
//  sendto(m_reportsocket, (const char *)rd_msg_1s, sizeof(rd_msg_1s), 0, (struct sockaddr *)&m_radar_addr, sizeof(m_radar_addr));
//}
//
//static uint8_t rd_msg_5s[] = {0x03, 0x89, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
//                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x01, 0x00, 0x00,
//                              0x9e, 0x03, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
//
//void RME120Receive::Send5sKeepalive() {
//  sendto(m_dataSocket, (const char *)rd_msg_5s, sizeof(rd_msg_5s), 0, (struct sockaddr *)&m_radar_addr, sizeof(m_radar_addr));
//}
//
//static uint8_t rd_msg_once[] = {0x02, 0x81, 0x01, 0x00, 0x7d, 0x00, 0x00, 0x00, 0xfa, 0x00, 0x00, 0x00, 0xf4, 0x01,
//                                0x00, 0x00, 0xee, 0x02, 0x00, 0x00, 0xdc, 0x05, 0x00, 0x00, 0xb8, 0x0b, 0x00, 0x00,
//                                0x70, 0x17, 0x00, 0x00, 0xe0, 0x2e, 0x00, 0x00, 0xc0, 0x5d, 0x00, 0x00, 0x80, 0xbb,
//                                0x00, 0x00, 0x40, 0x19, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
//                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
//
//static uint8_t wakeup_msg[] = "ABCDEFGHIJKLMNOP";
//
//void RME120Receive::WakeupRadar() {
//  if (!m_pi->m_settings.enable_transmit) return;
//
//  int outSD = socket(AF_INET, SOCK_DGRAM, 0);
//  if (outSD < 0) {
//    perror("Unable to create a socket");
//    return;
//  }
//
//  struct sockaddr_in groupSock;
//  memset((char *)&groupSock, 0, sizeof(groupSock));
//  groupSock.sin_family = AF_INET;
//  groupSock.sin_addr.s_addr = inet_addr("224.0.0.1");
//  groupSock.sin_port = htons(5800);
//
//  for (int i = 0; i < 10; i++) {
//    int res = sendto(outSD, (const char *)wakeup_msg, 16, 0, (struct sockaddr *)&groupSock, sizeof(groupSock));
//    if (res < 0) {
//      perror("send_revive, dying 1");
//    }
//    Sleep(10);
//  }
//
//  close(outSD);
//}



void RME120Receive::Shutdown() {
  if (m_send_socket != INVALID_SOCKET) {
    m_shutdown_time_requested = wxGetUTCTimeMillis();
    if (send(m_send_socket, "!", 1, MSG_DONTROUTE) > 0) {
      return;
    }
  }
  LOG_INFO(wxT("radar_pi: %s receive thread will take long time to stop"), m_ri->m_name.c_str());
}

wxString RME120Receive::GetInfoStatus() {
  wxCriticalSectionLocker lock(m_lock);
  // Called on the UI thread, so be gentle
  if (m_firmware.length() > 0) {
    return m_status + wxT("\n") + m_firmware;
  }
  return m_status;
}

PLUGIN_END_NAMESPACE
