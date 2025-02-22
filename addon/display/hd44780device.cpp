//
// hd44780device.cpp
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2018-2022  R. Stange <rsta2@o2online.de>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "hd44780device.h"
#include <circle/timer.h>
#include <assert.h>

enum TDisplayState
{
	ScreenStateStart,
	ScreenStateEscape,
	ScreenStateBracket,
	ScreenStateNumber1,
	ScreenStateQuestionMark,
	ScreenStateSemicolon,
	ScreenStateNumber2,
	ScreenStateNumber3,
	ScreenStateAutoPage
};

CHD44780Device::CHD44780Device (unsigned nColumns, unsigned nRows,
				unsigned nD4Pin, unsigned nD5Pin, unsigned nD6Pin, unsigned nD7Pin,
				unsigned nENPin, unsigned nRSPin, unsigned nRWPin,
				boolean bBlockCursor)
:	m_nColumns (nColumns),
	m_nRows (nRows),
	m_D4 (nD4Pin, GPIOModeOutput),
	m_D5 (nD5Pin, GPIOModeOutput),
	m_D6 (nD6Pin, GPIOModeOutput),
	m_D7 (nD7Pin, GPIOModeOutput),
	m_EN (nENPin, GPIOModeOutput),
	m_RS (nRSPin, GPIOModeOutput),
	m_pRW (0),
	m_pI2CMaster (0),
	m_nAddress (0),
	m_bBlockCursor (bBlockCursor),
	m_nState (ScreenStateStart),
	m_SpinLock (TASK_LEVEL)
{
	assert (m_nColumns <= HD44780_MAX_COLUMNS);
	assert (m_nRows <= HD44780_MAX_ROWS);

	m_EN.Write (LOW);
	m_RS.Write (LOW);

	if (nRWPin != 0)
	{
		m_pRW = new CGPIOPin (nRWPin, GPIOModeOutput);
		if (m_pRW != 0)
		{
			m_pRW->Write (LOW);	// set to pin write, we do not read
		}
	}
}

CHD44780Device::CHD44780Device (CI2CMaster *pI2CMaster, u8 nAddress,
				unsigned nColumns, unsigned nRows, boolean bBlockCursor)
:	m_nColumns (nColumns),
	m_nRows (nRows),
	m_pI2CMaster (pI2CMaster),
	m_nAddress (nAddress),
	m_bBlockCursor (bBlockCursor),
	m_nState (ScreenStateStart),
	m_SpinLock (TASK_LEVEL)
{
	assert (m_nColumns <= HD44780_MAX_COLUMNS);
	assert (m_nRows <= HD44780_MAX_ROWS);
}

CHD44780Device::~CHD44780Device (void)
{
	WriteByte (0x30, HD44780_CMD);	// return to 8-bit mode

	delete m_pRW;
	m_pRW = 0;
}

boolean CHD44780Device::Initialize (void)
{
	// set 4-bit mode, line count and font
	WriteHalfByte (0x02, HD44780_CMD);
	WriteByte (m_nRows == 1 ? 0x20 : 0x28, HD44780_CMD);

	SetCursorMode (TRUE);
	SetAutoPageMode (FALSE);
	CursorHome ();
	ClearDisplayEnd ();

	WriteByte (0x06, HD44780_CMD);	// set move cursor right, do not shift display

	return TRUE;
}

unsigned CHD44780Device::GetColumns (void) const
{
	return m_nColumns;
}

unsigned CHD44780Device::GetRows (void) const
{
	return m_nRows;
}

int CHD44780Device::Write (const void *pBuffer, size_t nCount)
{
	const char *pChar = (const char *) pBuffer;
	assert (pChar != 0);

	int nResult = 0;

	m_SpinLock.Acquire ();

	while (nCount-- > 0)
	{
		Write (*pChar++);

		nResult++;
	}

	SetCursor ();

	m_SpinLock.Release ();

	return nResult;
}

void CHD44780Device::Write (char chChar)
{
	switch (m_nState)
	{
	case ScreenStateStart:
		switch (chChar)
		{
		case '\b':
			CursorLeft ();
			break;

		case '\t':
			Tabulator ();
			break;

		case '\n':
			NewLine ();
			break;

		case '\r':
			CarriageReturn ();
			break;

		case '\x1b':
			m_nState = ScreenStateEscape;
			break;

		default:
			DisplayChar (chChar);
			break;
		}
		break;

	case ScreenStateEscape:
		switch (chChar)
		{
		case '[':
			m_nState = ScreenStateBracket;
			break;

		case 'd':
			m_nState = ScreenStateAutoPage;
			break;

		default:
			m_nState = ScreenStateStart;
			break;
		}
		break;

	case ScreenStateBracket:
		switch (chChar)
		{
		case '?':
			m_nState = ScreenStateQuestionMark;
			break;

		case 'A':
			CursorUp ();
			m_nState = ScreenStateStart;
			break;

		case 'B':
			CursorDown ();
			m_nState = ScreenStateStart;
			break;

		case 'C':
			CursorRight ();
			m_nState = ScreenStateStart;
			break;

		case 'D':
			CursorLeft ();
			m_nState = ScreenStateStart;
			break;

		case 'H':
			CursorHome ();
			m_nState = ScreenStateStart;
			break;

		case 'J':
			ClearDisplayEnd ();
			m_nState = ScreenStateStart;
			break;

		case 'K':
			ClearLineEnd ();
			m_nState = ScreenStateStart;
			break;

		default:
			if ('0' <= chChar && chChar <= '9')
			{
				m_nParam1 = chChar - '0';
				m_nState = ScreenStateNumber1;
			}
			else
			{
				m_nState = ScreenStateStart;
			}
			break;
		}
		break;

	case ScreenStateNumber1:
		switch (chChar)
		{
		case ';':
			m_nState = ScreenStateSemicolon;
			break;

		case 'X':
			EraseChars (m_nParam1);
			m_nState = ScreenStateStart;
			break;

		default:
			if ('0' <= chChar && chChar <= '9')
			{
				m_nParam1 *= 10;
				m_nParam1 += chChar - '0';

				if (m_nParam1 > 99)
				{
					m_nState = ScreenStateStart;
				}
			}
			else
			{
				m_nState = ScreenStateStart;
			}
			break;
		}
		break;

	case ScreenStateSemicolon:
		if ('0' <= chChar && chChar <= '9')
		{
			m_nParam2 = chChar - '0';
			m_nState = ScreenStateNumber2;
		}
		else
		{
			m_nState = ScreenStateStart;
		}
		break;

	case ScreenStateQuestionMark:
		if ('0' <= chChar && chChar <= '9')
		{
			m_nParam1 = chChar - '0';
			m_nState = ScreenStateNumber3;
		}
		else
		{
			m_nState = ScreenStateStart;
		}
		break;

	case ScreenStateNumber2:
		switch (chChar)
		{
		case 'H':
			CursorMove (m_nParam1, m_nParam2);
			m_nState = ScreenStateStart;
			break;

		default:
			if ('0' <= chChar && chChar <= '9')
			{
				m_nParam2 *= 10;
				m_nParam2 += chChar - '0';

				if (m_nParam2 > 199)
				{
					m_nState = ScreenStateStart;
				}
			}
			else
			{
				m_nState = ScreenStateStart;
			}
			break;
		}
		break;

	case ScreenStateNumber3:
		switch (chChar)
		{
		case 'h':
		case 'l':
			if (m_nParam1 == 25)
			{
				SetCursorMode (chChar == 'h');
			}
			m_nState = ScreenStateStart;
			break;

		default:
			if ('0' <= chChar && chChar <= '9')
			{
				m_nParam1 *= 10;
				m_nParam1 += chChar - '0';

				if (m_nParam1 > 99)
				{
					m_nState = ScreenStateStart;
				}
			}
			else
			{
				m_nState = ScreenStateStart;
			}
			break;
		}
		break;

	case ScreenStateAutoPage:
		switch (chChar)
		{
		case '+':
			SetAutoPageMode (TRUE);
			m_nState = ScreenStateStart;
			break;

		case '*':
			SetAutoPageMode (FALSE);
			m_nState = ScreenStateStart;
			break;

		default:
			m_nState = ScreenStateStart;
			break;
		}
		break;

	default:
		m_nState = ScreenStateStart;
		break;
	}
}

void CHD44780Device::CarriageReturn (void)
{
	m_nCursorX = 0;
}

void CHD44780Device::ClearDisplayEnd (void)
{
	if (m_nCursorX == 0 && m_nCursorY == 0)
	{
		WriteByte (0x01, HD44780_CMD);
		CTimer::SimpleMsDelay (2);

		for (unsigned nPosY = 0; nPosY < m_nRows; nPosY++)
		{
			for (unsigned nPosX = 0; nPosX < m_nColumns; nPosX++)
			{
				m_Buffer[nPosY][nPosX] = ' ';
			}
		}
	}
	else
	{
		ClearLineEnd ();

		for (unsigned nPosY = m_nCursorY+1; nPosY < m_nRows; nPosY++)
		{
			for (unsigned nPosX = 0; nPosX < m_nColumns; nPosX++)
			{
				SetChar (nPosX, nPosY, ' ');
			}
		}
	}
}

void CHD44780Device::ClearLineEnd (void)
{
	for (unsigned nPosX = m_nCursorX; nPosX < m_nColumns; nPosX++)
	{
		SetChar (nPosX, m_nCursorY, ' ');
	}
}

void CHD44780Device::CursorDown (void)
{
	if (++m_nCursorY >= m_nRows)
	{
		if (!m_bAutoPage)
		{
			Scroll ();

			m_nCursorY--;
		}
		else
		{
			m_nCursorY = 0;
		}
	}
}

void CHD44780Device::CursorHome (void)
{
	m_nCursorX = 0;
	m_nCursorY = 0;
}

void CHD44780Device::CursorLeft (void)
{
	if (m_nCursorX > 0)
	{
		m_nCursorX--;
	}
	else
	{
		if (m_nCursorY > 0)
		{
			m_nCursorX = m_nColumns-1;
			m_nCursorY--;
		}
	}
}

void CHD44780Device::CursorMove (unsigned nRow, unsigned nColumn)
{
	if (   --nColumn < m_nColumns
	    && --nRow    < m_nRows)
	{
		m_nCursorX = nColumn;
		m_nCursorY = nRow;
	}
}

void CHD44780Device::CursorRight (void)
{
	if (++m_nCursorX >= m_nColumns)
	{
		NewLine ();
	}
}

void CHD44780Device::CursorUp (void)
{
	if (m_nCursorY > 0)
	{
		m_nCursorY--;
	}
}

void CHD44780Device::DisplayChar (char chChar)
{
	u8 uchChar = (u8) chChar;

	if (' ' <= uchChar)
	{
		if (0x80 <= uchChar && uchChar <= 0x87)
		{
			uchChar -= 0x80;
		}

		SetChar (m_nCursorX, m_nCursorY, uchChar);

		CursorRight ();
	}
}

void CHD44780Device::EraseChars (unsigned nCount)
{
	if (nCount == 0)
	{
		return;
	}

	unsigned nEndX = m_nCursorX + nCount;
	if (nEndX > m_nColumns)
	{
		nEndX = m_nColumns;
	}

	for (unsigned nPosX = m_nCursorX; nPosX < nEndX; nPosX++)
	{
		SetChar (nPosX, m_nCursorY, ' ');
	}
}

void CHD44780Device::NewLine (void)
{
	CarriageReturn ();
	CursorDown ();
}

void CHD44780Device::SetAutoPageMode (boolean bEnable)
{
	m_bAutoPage = bEnable;
}

void CHD44780Device::SetCursorMode (boolean bVisible)
{
	m_bCursorOn = bVisible;

	WriteByte (m_bCursorOn ? (m_bBlockCursor ? 0x0D : 0x0E) : 0x0C, HD44780_CMD);
}

void CHD44780Device::Tabulator (void)
{
	m_nCursorX = ((m_nCursorX + 8) / 8) * 8;
	if (m_nCursorX >= m_nColumns)
	{
		NewLine ();
	}
}

void CHD44780Device::Scroll (void)
{
	for (unsigned nPosY = 1; nPosY < m_nRows; nPosY++)
	{
		for (unsigned nPosX = 0; nPosX < m_nColumns; nPosX++)
		{
			SetChar (nPosX, nPosY-1, m_Buffer[nPosY][nPosX]);
		}
	}

	for (unsigned nPosX = 0; nPosX < m_nColumns; nPosX++)
	{
		SetChar (nPosX, m_nRows-1, ' ');
	}
}

void CHD44780Device::SetChar (unsigned nPosX, unsigned nPosY, char chChar)
{
	WriteByte (0x80 | GetDDRAMAddress (nPosX, nPosY), HD44780_CMD);

	WriteByte ((u8) chChar, HD44780_DATA);

	m_Buffer[nPosY][nPosX] = chChar;
}

void CHD44780Device::SetCursor (void)
{
	if (m_bCursorOn)
	{
		WriteByte (0x80 | GetDDRAMAddress (m_nCursorX, m_nCursorY), HD44780_CMD);
	}
}

void CHD44780Device::DefineCharFont (char chChar, const u8 FontData[8])
{
	u8 uchChar = (u8) chChar;
	if (   uchChar < 0x80
	    || uchChar > 0x87)
	{
		return;
	}
	uchChar -= 0x80;

	u8 uchCGRAMAddress = uchChar << 3;

	for (unsigned nLine = 0; nLine <= 7; nLine++)
	{
		WriteByte (0x40 | (uchCGRAMAddress + nLine), HD44780_CMD);

		WriteByte (FontData[nLine] & 0x1F, HD44780_DATA);
	}
}

void CHD44780Device::WriteByte (u8 nData, int mode)
{
	WriteHalfByte (nData >> 4, mode);
	WriteHalfByte (nData & 0x0F, mode);
}

#define LCD_DATA_BIT		(1<<0)
#define LCD_ENABLE_BIT		(1<<2)
#define LCD_BACKLIGHT_BIT	(1<<3)

void CHD44780Device::WriteHalfByte (u8 nData, int mode)
{
	if (m_pI2CMaster) {
		// I2C interface
		u8 nByte = ((nData << 4) & 0xF0) | LCD_ENABLE_BIT;
		nByte |= LCD_BACKLIGHT_BIT;
		if (mode == HD44780_DATA) {
			nByte |= LCD_DATA_BIT;
		}

		m_pI2CMaster->Write(m_nAddress, &nByte, 1);
		CTimer::SimpleusDelay (5);

		nByte &= ~LCD_ENABLE_BIT;
		m_pI2CMaster->Write(m_nAddress, &nByte, 1);

		CTimer::SimpleusDelay (100);
	} else {
		// 4-pin data interface
		
		if (mode == HD44780_DATA) {
			m_RS.Write(HIGH);
		}
		m_D4.Write (nData & 1 ? HIGH : LOW);
		m_D5.Write (nData & 2 ? HIGH : LOW);
		m_D6.Write (nData & 4 ? HIGH : LOW);
		m_D7.Write (nData & 8 ? HIGH : LOW);

		m_EN.Write (HIGH);
		CTimer::SimpleusDelay (1);
		m_EN.Write (LOW);
		CTimer::SimpleusDelay (50);
		if (mode == HD44780_DATA) {
			m_RS.Write(LOW);
		}
	}
}

u8 CHD44780Device::GetDDRAMAddress (unsigned nPosX, unsigned nPosY)
{
	u8 uchAddress = nPosX;
	switch (nPosY)
	{
	case 0:
		break;

	case 1:
		uchAddress += 0x40;
		break;

	case 2:
		uchAddress += m_nColumns;
		break;

	case 3:
		uchAddress += 0x40 + m_nColumns;
		break;

	default:
		assert (0);
		break;
	}

	return uchAddress;
}
