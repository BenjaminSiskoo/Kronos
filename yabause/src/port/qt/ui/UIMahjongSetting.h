/*	Copyright 2026 The Kronos Team

	This file is part of Yabause.

	Yabause is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Yabause is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Yabause; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/
#ifndef UIMAHJONGSETTING_H
#define UIMAHJONGSETTING_H

#include "ui_UIMahjongSetting.h"
#include "UIControllerSetting.h"
#include "QtYabause.h"

#include <QMap>

class QTimer;

// Key-binding dialog for the Sega Mahjong Panel used by kiwames (Pro
// Mahjong Kiwame S, 2P), vmahjong (Virtual Mahjong, 1P) and myfairld
// (Virtual Mahjong 2 - My Fair Lady, 1P). See the row-scan mux comment
// block above PERMAHJONG_A in peripheral.h for the row/bit wiring this
// mirrors (taken bit-for-bit from MAME's sega/stv.cpp stvmp_ioga_r/w).
class UIMahjongSetting : public UIControllerSetting, public Ui::UIMahjongSetting
{
	Q_OBJECT

public:
	UIMahjongSetting( PerInterface_struct* core, uint port, uint pad, uint perType, QWidget* parent = 0 );
	virtual ~UIMahjongSetting();

protected:

protected slots:
};

#endif // UIMAHJONGSETTING_H
