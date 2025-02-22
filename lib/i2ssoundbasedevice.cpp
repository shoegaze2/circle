//
// i2ssoundbasedevice.cpp
//
// Supports:
//	BCM283x/BCM2711 I2S output and input
//	two 24-bit audio channels
//	sample rate up to 192 KHz
//	output tested with PCM5102A, PCM5122 and WM8960 DACs
//
// References:
//	https://www.raspberrypi.org/forums/viewtopic.php?f=44&t=8496
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2016-2022  R. Stange <rsta2@o2online.de>
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
#include <circle/i2ssoundbasedevice.h>
#include <circle/devicenameservice.h>
#include <circle/bcm2835.h>
#include <circle/bcm2835int.h>
#include <circle/memio.h>
#include <circle/timer.h>
#include <assert.h>

#define CHANS			2			// 2 I2S stereo channels
#define CHANLEN			32			// width of a channel slot in bits

//
// PCM / I2S registers
//
#define CS_A_STBY		(1 << 25)
#define CS_A_SYNC		(1 << 24)
#define CS_A_RXSEX		(1 << 23)
#define CS_A_TXE		(1 << 21)
#define CS_A_TXD		(1 << 19)
#define CS_A_TXW		(1 << 17)
#define CS_A_TXERR		(1 << 15)
#define CS_A_TXSYNC		(1 << 13)
#define CS_A_DMAEN		(1 << 9)
#define CS_A_TXTHR__SHIFT	5
#define CS_A_RXCLR		(1 << 4)
#define CS_A_TXCLR		(1 << 3)
#define CS_A_TXON		(1 << 2)
#define CS_A_RXON		(1 << 1)
#define CS_A_EN			(1 << 0)

#define MODE_A_CLKI		(1 << 22)
#define MODE_A_CLKM		(1 << 23)
#define MODE_A_FSI		(1 << 20)
#define MODE_A_FSM		(1 << 21)
#define MODE_A_FLEN__SHIFT	10
#define MODE_A_FSLEN__SHIFT	0

#define RXC_A_CH1WEX		(1 << 31)
#define RXC_A_CH1EN		(1 << 30)
#define RXC_A_CH1POS__SHIFT	20
#define RXC_A_CH1WID__SHIFT	16
#define RXC_A_CH2WEX		(1 << 15)
#define RXC_A_CH2EN		(1 << 14)
#define RXC_A_CH2POS__SHIFT	4
#define RXC_A_CH2WID__SHIFT	0

#define TXC_A_CH1WEX		(1 << 31)
#define TXC_A_CH1EN		(1 << 30)
#define TXC_A_CH1POS__SHIFT	20
#define TXC_A_CH1WID__SHIFT	16
#define TXC_A_CH2WEX		(1 << 15)
#define TXC_A_CH2EN		(1 << 14)
#define TXC_A_CH2POS__SHIFT	4
#define TXC_A_CH2WID__SHIFT	0

#define DREQ_A_TX__SHIFT	8
#define DREQ_A_TX__MASK		(0x7F << 8)
#define DREQ_A_RX__SHIFT	0
#define DREQ_A_RX__MASK		(0x7F << 0)

CI2SSoundBaseDevice::CI2SSoundBaseDevice (CInterruptSystem *pInterrupt,
					  unsigned	    nSampleRate,
					  unsigned	    nChunkSize,
					  bool		    bSlave,
					  CI2CMaster       *pI2CMaster,
					  u8                ucI2CAddress,
					  TDeviceMode       DeviceMode)
:	CSoundBaseDevice (SoundFormatSigned24_32, 0, nSampleRate),
	m_nChunkSize (nChunkSize),
	m_bSlave (bSlave),
	m_pI2CMaster (pI2CMaster),
	m_ucI2CAddress (ucI2CAddress),
	m_DeviceMode (DeviceMode),
	m_Clock (GPIOClockPCM, GPIOClockSourcePLLD),
	m_bI2CInited (FALSE),
	m_bError (FALSE),
	m_TXBuffers (TRUE, ARM_PCM_FIFO_A, DREQSourcePCMTX, nChunkSize, pInterrupt),
	m_RXBuffers (FALSE, ARM_PCM_FIFO_A, DREQSourcePCMRX, nChunkSize, pInterrupt)
{
	assert (m_nChunkSize >= 32);
	assert ((m_nChunkSize & 1) == 0);

	// start clock and I2S device
	if (!m_bSlave)
	{
		unsigned nClockFreq =
			CMachineInfo::Get ()->GetGPIOClockSourceRate (GPIOClockSourcePLLD);
		assert (nClockFreq > 0);
		assert (8000 <= nSampleRate && nSampleRate <= 192000);
		assert (nClockFreq % (CHANLEN*CHANS) == 0);
		unsigned nDivI = nClockFreq / (CHANLEN*CHANS) / nSampleRate;
		unsigned nTemp = nClockFreq / (CHANLEN*CHANS) % nSampleRate;
		unsigned nDivF = (nTemp * 4096 + nSampleRate/2) / nSampleRate;
		assert (nDivF <= 4096);
		if (nDivF > 4095)
		{
			nDivI++;
			nDivF = 0;
		}

		m_Clock.Start (nDivI, nDivF, nDivF > 0 ? 1 : 0);
	}

	RunI2S ();

	CDeviceNameService::Get ()->AddDevice ("sndi2s", this, FALSE);
}

CI2SSoundBaseDevice::~CI2SSoundBaseDevice (void)
{
	CDeviceNameService::Get ()->RemoveDevice ("sndi2s", FALSE);

	// stop I2S device and clock
	StopI2S ();
}

int CI2SSoundBaseDevice::GetRangeMin (void) const
{
	return -(1 << 23)+1;
}

int CI2SSoundBaseDevice::GetRangeMax (void) const
{
	return (1 << 23)-1;
}

boolean CI2SSoundBaseDevice::Start (void)
{
	if (m_bError)
	{
		return FALSE;
	}

	// optional DAC init via I2C
	if (   m_DeviceMode != DeviceModeRXOnly
	    && m_pI2CMaster != 0
	    && !m_bI2CInited)
	{
		if (m_ucI2CAddress != 0)
		{
			// fixed address, must succeed
			if (m_ucI2CAddress != 0x1A)
			{
				if (!InitPCM51xx (m_ucI2CAddress))
				{
					m_bError = TRUE;

					return FALSE;
				}
			}
			else
			{
				if (!InitWM8960 (m_ucI2CAddress))
				{
					m_bError = TRUE;

					return FALSE;
				}
			}
		}
		else
		{
			if (!InitPCM51xx (0x4C))		// auto probing, ignore failure
			{
				if (!InitPCM51xx (0x4D))
				{
					InitWM8960 (0x1A);
				}
			}
		}

		m_bI2CInited = TRUE;
	}

	// enable I2S DMA operation
	PeripheralEntry ();

	if (m_nChunkSize < 64)
	{
		assert (m_nChunkSize >= 32);
		if (m_DeviceMode != DeviceModeRXOnly)
		{
			write32 (ARM_PCM_DREQ_A,   (read32 (ARM_PCM_DREQ_A) & ~DREQ_A_TX__MASK)
						 | (0x18 << DREQ_A_TX__SHIFT));
		}

		if (m_DeviceMode != DeviceModeTXOnly)
		{
			write32 (ARM_PCM_DREQ_A,   (read32 (ARM_PCM_DREQ_A) & ~DREQ_A_RX__MASK)
						 | (0x18 << DREQ_A_RX__SHIFT));  // TODO
		}
	}

	write32 (ARM_PCM_CS_A, read32 (ARM_PCM_CS_A) | CS_A_DMAEN);

	PeripheralExit ();

	u32 nTXRXOn = 0;

	if (m_DeviceMode != DeviceModeRXOnly)
	{
		if (!m_TXBuffers.Start (TXCompletedHandler, this))
		{
			m_bError = TRUE;

			return FALSE;
		}

		nTXRXOn |= CS_A_TXON;
	}

	if (m_DeviceMode != DeviceModeTXOnly)
	{
		if (!m_RXBuffers.Start (RXCompletedHandler, this))
		{
			m_bError = TRUE;

			return FALSE;
		}

		nTXRXOn |= CS_A_RXON | CS_A_RXSEX;
	}

	// enable TX and/or RX
	PeripheralEntry ();

	write32 (ARM_PCM_CS_A, read32 (ARM_PCM_CS_A) | nTXRXOn);

	PeripheralExit ();

	return TRUE;
}

void CI2SSoundBaseDevice::Cancel (void)
{
	if (m_DeviceMode != DeviceModeRXOnly)
	{
		m_TXBuffers.Cancel ();
	}

	if (m_DeviceMode != DeviceModeTXOnly)
	{
		m_RXBuffers.Cancel ();
	}
}

boolean CI2SSoundBaseDevice::IsActive (void) const
{
	if (   m_DeviceMode != DeviceModeRXOnly
	    && m_TXBuffers.IsActive ())
	{
		return TRUE;
	}

	if (   m_DeviceMode != DeviceModeTXOnly
	    && m_RXBuffers.IsActive ())
	{
		return TRUE;
	}

	return FALSE;
}

void CI2SSoundBaseDevice::RunI2S (void)
{
	PeripheralEntry ();

	// disable I2S
	write32 (ARM_PCM_CS_A, 0);
	CTimer::Get ()->usDelay (10);

	// clearing FIFOs
	write32 (ARM_PCM_CS_A, read32 (ARM_PCM_CS_A) | CS_A_TXCLR | CS_A_RXCLR);
	CTimer::Get ()->usDelay (10);

	// enable channel 1 and 2
	write32 (ARM_PCM_TXC_A,   TXC_A_CH1WEX
				| TXC_A_CH1EN
				| (1 << TXC_A_CH1POS__SHIFT)
				| (0 << TXC_A_CH1WID__SHIFT)
				| TXC_A_CH2WEX
				| TXC_A_CH2EN
				| ((CHANLEN+1) << TXC_A_CH2POS__SHIFT)
				| (0 << TXC_A_CH2WID__SHIFT));

	write32 (ARM_PCM_RXC_A,   RXC_A_CH1WEX
				| RXC_A_CH1EN
				| (1 << RXC_A_CH1POS__SHIFT)
				| (0 << RXC_A_CH1WID__SHIFT)
				| RXC_A_CH2WEX
				| RXC_A_CH2EN
				| ((CHANLEN+1) << RXC_A_CH2POS__SHIFT)
				| (0 << RXC_A_CH2WID__SHIFT));

	u32 nModeA =   MODE_A_CLKI
		     | MODE_A_FSI
		     | ((CHANS*CHANLEN-1) << MODE_A_FLEN__SHIFT)
		     | (CHANLEN << MODE_A_FSLEN__SHIFT);

	// set PCM clock and frame sync as inputs if in slave mode
	if (m_bSlave)
	{
		nModeA |= MODE_A_CLKM | MODE_A_FSM;
	}

	write32 (ARM_PCM_MODE_A, nModeA);

	// init GPIO pins
	unsigned nPinBase = 18;
	TGPIOMode GPIOMode = GPIOModeAlternateFunction0;

	// assign to P5 header on early models
	TMachineModel Model = CMachineInfo::Get ()->GetMachineModel ();
	if (   Model == MachineModelA
	    || Model == MachineModelBRelease2MB256
	    || Model == MachineModelBRelease2MB512)
	{
		nPinBase = 28;
		GPIOMode = GPIOModeAlternateFunction2;
	}

	m_PCMCLKPin.AssignPin (nPinBase);
	m_PCMCLKPin.SetMode (GPIOMode);
	m_PCMFSPin.AssignPin (nPinBase+1);
	m_PCMFSPin.SetMode (GPIOMode);

	if (m_DeviceMode != DeviceModeTXOnly)
	{
		m_PCMDINPin.AssignPin (nPinBase+2);
		m_PCMDINPin.SetMode (GPIOMode);
	}

	if (m_DeviceMode != DeviceModeRXOnly)
	{
		m_PCMDOUTPin.AssignPin (nPinBase+3);
		m_PCMDOUTPin.SetMode (GPIOMode);
	}

	// disable standby
	write32 (ARM_PCM_CS_A, read32 (ARM_PCM_CS_A) | CS_A_STBY);
	CTimer::Get ()->usDelay (50);

	// enable I2S
	write32 (ARM_PCM_CS_A, read32 (ARM_PCM_CS_A) | CS_A_EN);
	CTimer::Get ()->usDelay (10);

	PeripheralExit ();
}

void CI2SSoundBaseDevice::StopI2S (void)
{
	PeripheralEntry ();

	write32 (ARM_PCM_CS_A, 0);
	CTimer::Get ()->usDelay (50);

	PeripheralExit ();

	if (!m_bSlave)
	{
		m_Clock.Stop ();
	}

	// de-init GPIO pins
	m_PCMCLKPin.SetMode (GPIOModeInput);
	m_PCMFSPin.SetMode (GPIOModeInput);

	if (m_DeviceMode != DeviceModeTXOnly)
	{
		m_PCMDINPin.SetMode (GPIOModeInput);
	}

	if (m_DeviceMode != DeviceModeRXOnly)
	{
		m_PCMDOUTPin.SetMode (GPIOModeInput);
	}
}

unsigned CI2SSoundBaseDevice::TXCompletedHandler (boolean bStatus, u32 *pBuffer,
						  unsigned nChunkSize, void *pParam)
{
	CI2SSoundBaseDevice *pThis = (CI2SSoundBaseDevice *) pParam;
	assert (pThis != 0);

	if (!bStatus)
	{
		pThis->m_bError = TRUE;

		return 0;
	}

	return pThis->GetChunk (pBuffer, nChunkSize);
}

unsigned CI2SSoundBaseDevice::RXCompletedHandler (boolean bStatus, u32 *pBuffer,
						  unsigned nChunkSize, void *pParam)
{
	CI2SSoundBaseDevice *pThis = (CI2SSoundBaseDevice *) pParam;
	assert (pThis != 0);

	if (!bStatus)
	{
		pThis->m_bError = TRUE;

		return 0;
	}

	pThis->PutChunk (pBuffer, nChunkSize);

	return 0;
}

//
// Taken from the file mt32pi.cpp from this project:
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2021 Dale Whinham <daleyo@gmail.com>
//
// Licensed under GPLv3
//
boolean CI2SSoundBaseDevice::InitPCM51xx (u8 ucI2CAddress)
{
	static const u8 initBytes[][2] =
	{
		// Set PLL reference clock to BCK (set SREF to 001b)
		{ 0x0d, 0x10 },

		// Ignore clock halt detection (set IDCH to 1)
		{ 0x25, 0x08 },

		// Disable auto mute
		{ 0x41, 0x04 }
	};

	for (auto &command : initBytes)
	{
		if (   m_pI2CMaster->Write (ucI2CAddress, &command, sizeof (command))
		    != sizeof (command))
		{
			return FALSE;
		}
	}

	return TRUE;
}

// For WM8960 i2c register is 7 bits and value is 9 bits,
// so let's have a helper for packing this into two bytes
#define SHIFT_BIT(r, v) {((v&0x0100)>>8) | (r<<1), (v&0xff)}

boolean CI2SSoundBaseDevice::InitWM8960 (u8 ucI2CAddress)
{
	// based on https://github.com/RASPIAUDIO/ULTRA/blob/main/ultra.c
	// Licensed under GPLv3
	static const u8 initBytes[][2] =
	{
		// reset
		SHIFT_BIT(15, 0x000),
		// Power
		SHIFT_BIT(25, 0x1FC),
		SHIFT_BIT(26, 0x1F9),
		SHIFT_BIT(47, 0x03C),
		// Clock PLL
		SHIFT_BIT(4, 0x001),
		SHIFT_BIT(52, 0x027),
		SHIFT_BIT(53, 0x086),
		SHIFT_BIT(54, 0x0C2),
		SHIFT_BIT(55, 0x026),
		// ADC/DAC
		SHIFT_BIT(5, 0x000),
		SHIFT_BIT(7, 0x002),
		// ALC and Noise control
		SHIFT_BIT(20, 0x0F9),
		SHIFT_BIT(17, 0x1FB),
		SHIFT_BIT(18, 0x000),
		SHIFT_BIT(19, 0x032),
		// OUT1 volume
		SHIFT_BIT(2, 0x16F),
		SHIFT_BIT(3, 0x16F),
		//SPK volume
		SHIFT_BIT(40, 0x17F),
		SHIFT_BIT(41, 0x178),
		SHIFT_BIT(51, 0x08D),
		// input volume
		SHIFT_BIT(0, 0x13F),
		SHIFT_BIT(1, 0x13F),
		// INPUTS
		SHIFT_BIT(32, 0x138),
		SHIFT_BIT(33, 0x138),
		// OUTPUTS
		SHIFT_BIT(49, 0x0F7),
		SHIFT_BIT(10, 0x1FF),
		SHIFT_BIT(11, 0x1FF),
		SHIFT_BIT(34, 0x100),
		SHIFT_BIT(37, 0x100)
	};

	for (auto &command : initBytes)
	{
		if (   m_pI2CMaster->Write (ucI2CAddress, &command, sizeof (command))
		    != sizeof (command))
		{
			return FALSE;
		}
	}

	return TRUE;
}
