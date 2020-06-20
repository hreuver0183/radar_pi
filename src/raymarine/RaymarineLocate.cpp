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

#include "raymarine/RaymarineLocate.h"

PLUGIN_BEGIN_NAMESPACE

//
// Raymarine E120 radars and compatible report their addresses here, (including the version?) #.
//
static const NetworkAddress reportRaymarineCommon(224, 0, 0, 1, 5800);

#define SECONDS_PER_SELECT (1)
#define PERIOD_UNTIL_CARD_REFRESH (60)
#define PERIOD_UNTIL_WAKE_RADAR (30)

void RaymarineLocate::CleanupCards() {
  if (m_interface_addr) {
    delete[] m_interface_addr;
    m_interface_addr = 0;
  }
  if (m_socket) {
    for (size_t i = 0; i < m_interface_count; i++) {
      if (m_socket[i] != INVALID_SOCKET) {
        closesocket(m_socket[i]);
      }
    }
    delete[] m_socket;
    m_socket = 0;
  }
  m_interface_count = 0;
}

void RaymarineLocate::UpdateEthernetCards() {
  struct ifaddrs *addr_list;
  struct ifaddrs *addr;
  size_t i = 0;
  wxString error;

  CleanupCards();

  if (!getifaddrs(&addr_list)) {
    // Count the # of active IPv4 cards
    for (addr = addr_list; addr; addr = addr->ifa_next) {
      if (VALID_IPV4_ADDRESS(addr)) {
        m_interface_count++;
      }
    }

    // If there are any fill packed array (m_socket, m_interface_addr) with them.
    if (m_interface_count > 0) {
      m_socket = new SOCKET[m_interface_count];
      m_interface_addr = new NetworkAddress[m_interface_count];

      for (addr = addr_list; addr; addr = addr->ifa_next) {
        if (VALID_IPV4_ADDRESS(addr)) {
          struct sockaddr_in *sa = (struct sockaddr_in *)addr->ifa_addr;
          m_interface_addr[i].addr = sa->sin_addr;
          m_interface_addr[i].port = 0;
          m_socket[i] = startUDPMulticastReceiveSocket(m_interface_addr[i], reportRaymarineCommon, error);
          LOG_VERBOSE(wxT("radar_pi: RaymarineLocate scanning interface %s for radars"), m_interface_addr[i].FormatNetworkAddress());
          i++;
        }
      }
    }

    freeifaddrs(addr_list);
  }

  //WakeRadar(); not  known for Raymarine
}

/*
 * Entry
 *
 * Called by wxThread when the new thread is running.
 * It should remain running until Shutdown is called.
 */
void *RaymarineLocate::Entry(void) {
  int r = 0;
  int rescan_network_cards = 0;
  bool success = false;

  union {
    sockaddr_storage addr;
    sockaddr_in ipv4;
  } rx_addr;
  socklen_t rx_len;

  uint8_t data[1500];

  LOG_INFO(wxT("radar_pi: $$$RaymarineLocate thread starting"));

  m_is_shutdown = false;

  UpdateEthernetCards();

  while (!m_shutdown) {
    struct timeval tv = { (long)1, (long)(0) };
    fd_set fdin;
    FD_ZERO(&fdin);

    int maxFd = INVALID_SOCKET;
    for (size_t i = 0; i < m_interface_count; i++) {
      if (m_socket[i] != INVALID_SOCKET) {
        FD_SET(m_socket[i], &fdin);
        maxFd = MAX(m_socket[i], maxFd);
      }
    }

    r = select(maxFd + 1, &fdin, 0, 0, &tv);
    if (r == -1 && errno != 0) {
      int err = errno;
      UpdateEthernetCards();
      rescan_network_cards = 0;
    }
    if (r > 0) {
      for (size_t i = 0; i < m_interface_count; i++) {
        if (m_socket[i] != INVALID_SOCKET && FD_ISSET(m_socket[i], &fdin)) {
          rx_len = sizeof(rx_addr);
          r = recvfrom(m_socket[i], (char *)data, sizeof(data), 0, (struct sockaddr *)&rx_addr, &rx_len);
          if (r > 2) {   // we are not interested in 2 byte messages
            NetworkAddress radar_address;
            radar_address.addr = rx_addr.ipv4.sin_addr;
            radar_address.port = rx_addr.ipv4.sin_port;

            if (ProcessReport(radar_address, m_interface_addr[i], data, (size_t)r)) {
              rescan_network_cards = -PERIOD_UNTIL_CARD_REFRESH;  // Give double time until we rescan
            }
            success = true;
            LOG_INFO(wxT("$$$a locate success"));
          }
        }
      }
    }
    else {  // no data received -> select timeout
      if (++rescan_network_cards >= PERIOD_UNTIL_CARD_REFRESH) {
        UpdateEthernetCards();
        rescan_network_cards = 0;        
      }
    }

  }  // endless loop until thread destroy

  LOG_INFO(wxT("radar_pi: $$$RaymarineLocate thread stopping"));

  CleanupCards();

  LOG_VERBOSE(wxT("radar_pi: $$$RaymarineLocate thread stopping"));
  m_is_shutdown = true;
  return 0;
}


#pragma pack(push, 1)

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
#pragma pack(pop)

bool RaymarineLocate::ProcessReport(const NetworkAddress &radar_address, const NetworkAddress &interface_address,
                                    const uint8_t *report, size_t len) {
  
   // LOG_INFO(wxT("radar_pi: $$$Wake radar request from %s"), radar_address.FormatNetworkAddress());

  SRMRadarFunc *rRec = (SRMRadarFunc *)report;
   wxCriticalSectionLocker lock(m_exclusive);
  
  if (len == sizeof(SRMRadarFunc) && rRec->func_id == 1) {  // only length 36 is processed with id==1, others (28, 37, 40, 56) to be investigated
    if (m_pi->m_settings.verbose >= 0 /* was 2*/) {
      LOG_BINARY_RECEIVE(wxT("radar_pi: $$$ RaymarineLocate received RadarReport"), report, len);
    }
    
    // radar_address.addr.s_addr = ntohl(rRec->radar_ip);
    // radar_address.port = ntohs(rRec->radar_port);

    RadarLocationInfo infoA;
    wxString sernr = wxT(" ");
    infoA.serialNr = sernr;  // empty
   // infoA.spoke_data_addr = NetworkAddress(rRec->radar_ip);
    infoA.spoke_data_addr.addr.s_addr = ntohl(rRec->mcast_ip);
    infoA.spoke_data_addr.port = ntohs(rRec->mcast_port);
    infoA.report_addr.addr.s_addr = ntohl(rRec->mcast_ip);
    infoA.report_addr.port = ntohs(rRec->mcast_port);
    infoA.send_command_addr.addr.s_addr = ntohl(rRec->mcast_ip);
    infoA.send_command_addr.port = ntohs(rRec->mcast_port);
   /* infoA.report_addr = NetworkAddress(rRec->radar_ip);
    infoA.send_command_addr = NetworkAddress(data->addrSendA);*/
    NetworkAddress radar_ipA = radar_address;
    radar_ipA.port = htons(RO_PRIMARY);
    if (m_report_count < MAX_REPORT) {
      LOG_INFO(wxT("radar_pi: $$$Located radar IP %s, interface %s [%s]"), radar_ipA.FormatNetworkAddressPort(), interface_address.FormatNetworkAddress(), infoA.to_string());
      m_report_count++;
    }
    else {
      LOG_RECEIVE(wxT("radar_pi: $$$Located radar IP %s, interface %s [%s]"), radar_ipA.FormatNetworkAddressPort(), interface_address.FormatNetworkAddress(), infoA.to_string());
    }
    LOG_INFO(wxT("radar_pi: $$$ radar_ipA"));
    m_pi->FoundRaymarineRadarInfo(radar_ipA, interface_address, infoA);
    return true;
  }

  //LOG_BINARY_RECEIVE(wxT("radar_pi: RaymarineLocate received unknown message"), report, len);
  return false;
}

//void RaymarineLocate::WakeRadar() {
//  static const uint8_t WAKE_COMMAND[] = { 0x01, 0xb1 };
//  struct sockaddr_in send_addr = NetworkAddress(236, 6, 7, 5, 6878).GetSockAddrIn();
//
//  int one = 1;
//
//  for (size_t i = 0; i < m_interface_count; i++) {
//    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
//    struct sockaddr_in s = m_interface_addr[i].GetSockAddrIn();
//
//    if (sock != INVALID_SOCKET) {
//      if (!setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one)) &&
//        !::bind(sock, (struct sockaddr *)&s, sizeof(s)) &&
//        sendto(sock, (const char *)WAKE_COMMAND, sizeof WAKE_COMMAND, 0, (struct sockaddr *)&send_addr, sizeof(send_addr)) ==
//        sizeof WAKE_COMMAND) {
//        LOG_VERBOSE(wxT("radar_pi: Sent wake command to radar on %s"), m_interface_addr[i].FormatNetworkAddress());
//      }
//      else {
//        wxLogError(wxT("radar_pi: Failed to send wake command to radars on %s"), m_interface_addr[i].FormatNetworkAddress());
//      }
//      closesocket(sock);
//    }
//  }
//}

PLUGIN_END_NAMESPACE
