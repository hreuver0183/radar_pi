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

//#define SEATALK_HS_ANNOUNCE_GROUP	"224.0.0.1"
//#define SEATALK_HS_ANNOUNCE_PORT	5800

RME120Control::RME120Control() {
  /*m_radar_socket = INVALID_SOCKET;
  m_name = wxT("Navico radar");*/
}

RME120Control::~RME120Control() {
  /*if (m_radar_socket != INVALID_SOCKET) {
    closesocket(m_radar_socket);
    LOG_TRANSMIT(wxT("radar_pi: %s transmit socket closed"), m_name.c_str());
  }*/
  int i = 0;
  i++;
}

// void NavicoControl::SetMultiCastAddress(NetworkAddress sendMultiCastAddress) { m_addr = sendMultiCastAddress.GetSockAddrIn(); }

bool RME120Control::Init(radar_pi *pi, RadarInfo *ri, NetworkAddress &ifadr, NetworkAddress &radaradr) { 
  return true;
 }

void RME120Control::RadarTxOff(){};
void RME120Control::RadarTxOn(){};
bool RME120Control::RadarStayAlive() { return true; };
bool RME120Control::SetRange(int meters) { return true; };

bool RME120Control::SetControlValue(ControlType controlType, RadarControlItem &item, RadarControlButton *button) { return true; };

  PLUGIN_END_NAMESPACE
