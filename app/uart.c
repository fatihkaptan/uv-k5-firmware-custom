/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <string.h>

#if !defined(ENABLE_OVERLAY)
	#include "ARMCM0.h"
#endif
#ifdef ENABLE_FMRADIO
	#include "app/fm.h"
#endif
#include "app/uart.h"
#include "board.h"
#include "bsp/dp32g030/dma.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/aes.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/crc.h"
#include "driver/eeprom.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"
#include "version.h"
#ifdef ENABLE_UART_CAT
	#include "frequencies.h"
	#include "helper/battery.h"
	#include "radio.h"
#endif

#if defined(ENABLE_OVERLAY)
	#include "sram-overlay.h"
#endif


#define DMA_INDEX(x, y) (((x) + (y)) % sizeof(UART_DMA_Buffer))

typedef struct {
	uint16_t ID;
	uint16_t Size;
} Header_t;

typedef struct {
	uint8_t  Padding[2];
	uint16_t ID;
} Footer_t;

typedef struct {
	Header_t Header;
	uint32_t Timestamp;
} CMD_0514_t;

typedef struct {
	Header_t Header;
	struct {
		char     Version[16];
		bool     bHasCustomAesKey;
		bool     bIsInLockScreen;
		uint8_t  Padding[2];
		uint32_t Challenge[4];
	} Data;
} REPLY_0514_t;

typedef struct {
	Header_t Header;
	uint16_t Offset;
	uint8_t  Size;
	uint8_t  Padding;
	uint32_t Timestamp;
} CMD_051B_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t Offset;
		uint8_t  Size;
		uint8_t  Padding;
		uint8_t  Data[128];
	} Data;
} REPLY_051B_t;

typedef struct {
	Header_t Header;
	uint16_t Offset;
	uint8_t  Size;
	bool     bAllowPassword;
	uint32_t Timestamp;
	uint8_t  Data[0];
} CMD_051D_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t Offset;
	} Data;
} REPLY_051D_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t RSSI;
		uint8_t  ExNoiseIndicator;
		uint8_t  GlitchIndicator;
	} Data;
} REPLY_0527_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t Voltage;
		uint16_t Current;
	} Data;
} REPLY_0529_t;

typedef struct {
	Header_t Header;
	uint32_t Response[4];
} CMD_052D_t;

typedef struct {
	Header_t Header;
	struct {
		bool bIsLocked;
		uint8_t Padding[3];
	} Data;
} REPLY_052D_t;

typedef struct {
	Header_t Header;
	uint32_t Timestamp;
} CMD_052F_t;

static const uint8_t Obfuscation[16] =
{
	0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40, 0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80
};

static union
{
	uint8_t Buffer[256];
	struct
	{
		Header_t Header;
		uint8_t Data[252];
	};
} UART_Command;

static uint32_t Timestamp;
static uint16_t gUART_WriteIndex;
static bool     bIsEncrypted = true;

static void SendReply(void *pReply, uint16_t Size)
{
	Header_t Header;
	Footer_t Footer;

	if (bIsEncrypted)
	{
		uint8_t     *pBytes = (uint8_t *)pReply;
		unsigned int i;
		for (i = 0; i < Size; i++)
			pBytes[i] ^= Obfuscation[i % 16];
	}

	Header.ID = 0xCDAB;
	Header.Size = Size;
	UART_Send(&Header, sizeof(Header));
	UART_Send(pReply, Size);

	if (bIsEncrypted)
	{
		Footer.Padding[0] = Obfuscation[(Size + 0) % 16] ^ 0xFF;
		Footer.Padding[1] = Obfuscation[(Size + 1) % 16] ^ 0xFF;
	}
	else
	{
		Footer.Padding[0] = 0xFF;
		Footer.Padding[1] = 0xFF;
	}
	Footer.ID = 0xBADC;

	UART_Send(&Footer, sizeof(Footer));
}

static void SendVersion(void)
{
	REPLY_0514_t Reply;

	Reply.Header.ID = 0x0515;
	Reply.Header.Size = sizeof(Reply.Data);
	strcpy(Reply.Data.Version, Version);
	Reply.Data.bHasCustomAesKey = bHasCustomAesKey;
	Reply.Data.bIsInLockScreen = bIsInLockScreen;
	Reply.Data.Challenge[0] = gChallenge[0];
	Reply.Data.Challenge[1] = gChallenge[1];
	Reply.Data.Challenge[2] = gChallenge[2];
	Reply.Data.Challenge[3] = gChallenge[3];

	SendReply(&Reply, sizeof(Reply));
}

static bool IsBadChallenge(const uint32_t *pKey, const uint32_t *pIn, const uint32_t *pResponse)
{
	unsigned int i;
	uint32_t     IV[4];

	IV[0] = 0;
	IV[1] = 0;
	IV[2] = 0;
	IV[3] = 0;

	AES_Encrypt(pKey, IV, pIn, IV, true);

	for (i = 0; i < 4; i++)
		if (IV[i] != pResponse[i])
			return true;

	return false;
}

// session init, sends back version info and state
// timestamp is a session id really
static void CMD_0514(const uint8_t *pBuffer)
{
	const CMD_0514_t *pCmd = (const CMD_0514_t *)pBuffer;

	Timestamp = pCmd->Timestamp;

	#ifdef ENABLE_FMRADIO
		gFmRadioCountdown_500ms = fm_radio_countdown_500ms;
	#endif

	gSerialConfigCountDown_500ms = 12; // 6 sec
	
	// turn the LCD backlight off
	BACKLIGHT_TurnOff();

	SendVersion();
}

// read eeprom
static void CMD_051B(const uint8_t *pBuffer)
{
	const CMD_051B_t *pCmd = (const CMD_051B_t *)pBuffer;
	REPLY_051B_t      Reply;
	bool              bLocked = false;

	if (pCmd->Timestamp != Timestamp)
		return;

	gSerialConfigCountDown_500ms = 12; // 6 sec

	#ifdef ENABLE_FMRADIO
		gFmRadioCountdown_500ms = fm_radio_countdown_500ms;
	#endif

	memset(&Reply, 0, sizeof(Reply));
	Reply.Header.ID   = 0x051C;
	Reply.Header.Size = pCmd->Size + 4;
	Reply.Data.Offset = pCmd->Offset;
	Reply.Data.Size   = pCmd->Size;

	if (bHasCustomAesKey)
		bLocked = gIsLocked;

	if (!bLocked)
		EEPROM_ReadBuffer(pCmd->Offset, Reply.Data.Data, pCmd->Size);

	SendReply(&Reply, pCmd->Size + 8);
}

// write eeprom
static void CMD_051D(const uint8_t *pBuffer)
{
	const CMD_051D_t *pCmd = (const CMD_051D_t *)pBuffer;
	REPLY_051D_t Reply;
	bool bReloadEeprom;
	bool bIsLocked;

	if (pCmd->Timestamp != Timestamp)
		return;

	gSerialConfigCountDown_500ms = 12; // 6 sec
	
	bReloadEeprom = false;

	#ifdef ENABLE_FMRADIO
		gFmRadioCountdown_500ms = fm_radio_countdown_500ms;
	#endif

	Reply.Header.ID   = 0x051E;
	Reply.Header.Size = sizeof(Reply.Data);
	Reply.Data.Offset = pCmd->Offset;

	bIsLocked = bHasCustomAesKey ? gIsLocked : bHasCustomAesKey;

	if (!bIsLocked)
	{
		unsigned int i;
		for (i = 0; i < (pCmd->Size / 8); i++)
		{
			const uint16_t Offset = pCmd->Offset + (i * 8U);

			if (Offset >= 0x0F30 && Offset < 0x0F40)
				if (!gIsLocked)
					bReloadEeprom = true;

			if ((Offset < 0x0E98 || Offset >= 0x0EA0) || !bIsInLockScreen || pCmd->bAllowPassword)
				EEPROM_WriteBuffer(Offset, &pCmd->Data[i * 8U]);
		}

		if (bReloadEeprom)
			SETTINGS_InitEEPROM();
	}

	SendReply(&Reply, sizeof(Reply));
}

// read RSSI
static void CMD_0527(void)
{
	REPLY_0527_t Reply;

	Reply.Header.ID             = 0x0528;
	Reply.Header.Size           = sizeof(Reply.Data);
	Reply.Data.RSSI             = BK4819_ReadRegister(BK4819_REG_67) & 0x01FF;
	Reply.Data.ExNoiseIndicator = BK4819_ReadRegister(BK4819_REG_65) & 0x007F;
	Reply.Data.GlitchIndicator  = BK4819_ReadRegister(BK4819_REG_63);

	SendReply(&Reply, sizeof(Reply));
}

// read ADC
static void CMD_0529(void)
{
	REPLY_0529_t Reply;

	Reply.Header.ID   = 0x52A;
	Reply.Header.Size = sizeof(Reply.Data);

	// Original doesn't actually send current!
	BOARD_ADC_GetBatteryInfo(&Reply.Data.Voltage, &Reply.Data.Current);

	SendReply(&Reply, sizeof(Reply));
}

static void CMD_052D(const uint8_t *pBuffer)
{
	const CMD_052D_t *pCmd = (const CMD_052D_t *)pBuffer;
	REPLY_052D_t      Reply;
	bool              bIsLocked;

	#ifdef ENABLE_FMRADIO
		gFmRadioCountdown_500ms = fm_radio_countdown_500ms;
	#endif
	Reply.Header.ID   = 0x052E;
	Reply.Header.Size = sizeof(Reply.Data);

	bIsLocked = bHasCustomAesKey;

	if (!bIsLocked)
		bIsLocked = IsBadChallenge(gCustomAesKey, gChallenge, pCmd->Response);

	if (!bIsLocked)
	{
		bIsLocked = IsBadChallenge(gDefaultAesKey, gChallenge, pCmd->Response);
		if (bIsLocked)
			gTryCount++;
	}

	if (gTryCount < 3)
	{
		if (!bIsLocked)
			gTryCount = 0;
	}
	else
	{
		gTryCount = 3;
		bIsLocked = true;
	}
	
	gIsLocked            = bIsLocked;
	Reply.Data.bIsLocked = bIsLocked;

	SendReply(&Reply, sizeof(Reply));
}

// session init, sends back version info and state
// timestamp is a session id really
// this command also disables dual watch, crossband, 
// DTMF side tones, freq reverse, PTT ID, DTMF decoding, frequency offset
// exits power save, sets main VFO to upper,
static void CMD_052F(const uint8_t *pBuffer)
{
	const CMD_052F_t *pCmd = (const CMD_052F_t *)pBuffer;

	gEeprom.DUAL_WATCH                               = DUAL_WATCH_OFF;
	gEeprom.CROSS_BAND_RX_TX                         = CROSS_BAND_OFF;
	gEeprom.RX_VFO                                   = 0;
	gEeprom.DTMF_SIDE_TONE                           = false;
	gEeprom.VfoInfo[0].FrequencyReverse              = false;
	gEeprom.VfoInfo[0].pRX                           = &gEeprom.VfoInfo[0].freq_config_RX;
	gEeprom.VfoInfo[0].pTX                           = &gEeprom.VfoInfo[0].freq_config_TX;
	gEeprom.VfoInfo[0].TX_OFFSET_FREQUENCY_DIRECTION = TX_OFFSET_FREQUENCY_DIRECTION_OFF;
	gEeprom.VfoInfo[0].DTMF_PTT_ID_TX_MODE           = PTT_ID_OFF;
#ifdef ENABLE_DTMF_CALLING
	gEeprom.VfoInfo[0].DTMF_DECODING_ENABLE          = false;
#endif

	#ifdef ENABLE_NOAA
		gIsNoaaMode = false;
	#endif

	if (gCurrentFunction == FUNCTION_POWER_SAVE)
		FUNCTION_Select(FUNCTION_FOREGROUND);

	gSerialConfigCountDown_500ms = 12; // 6 sec

	Timestamp = pCmd->Timestamp;

	// turn the LCD backlight off
	BACKLIGHT_TurnOff();

	SendVersion();
}

#ifdef ENABLE_UART_CAT
// ---------------------------------------------------------------------------
// UART CAT: PC control while the radio runs normally.
//
//  PC -> radio                       radio -> PC
//  0x0701 status request             0x0702 status
//  0x0703 PTT {on}                   0x0704 PTT ack
//  0x0705 set VFO frequency {f}      0x0706 frequency ack
//                                    0x0710 RX start event (unsolicited)
//                                    0x0711 RX end event   (unsolicited)
//
// Any 0x07xx command arms the RX events; a 0x0514 (programming hello) disarms
// them so CHIRP sessions never see unsolicited packets.
// Frequencies are in 10 Hz units, as everywhere else in the firmware.
// CAT commands never touch gSerialConfigCountDown_500ms, so they do not
// inhibit or cut TX like the programming commands do.
// ---------------------------------------------------------------------------

#define CAT_PTT_TIMEOUT_10ms 200   // PTT drops if not refreshed within 2 s

enum {
	CAT_OK = 0,
	CAT_ERR_NOT_VFO,       // selected VFO is a memory channel
	CAT_ERR_BUSY,          // transmitting / PTT held
	CAT_ERR_RANGE,         // frequency outside RX range
	CAT_ERR_BAND,          // band change not supported over CAT
	CAT_ERR_REJECTED,      // radio refused (reduced service etc.)
};

typedef struct {
	Header_t Header;
	struct {
		uint32_t RxFrequency;
		uint32_t TxFrequency;
		uint16_t RSSI;
		uint16_t Battery_10mV;
		uint8_t  Function;
		uint8_t  RxVfo;
		uint8_t  TxVfo;
		uint8_t  Flags;     // b0 RX active, b1 CAT PTT, b2 PTT pressed, b3 events armed
		uint8_t  Noise;
		uint8_t  Glitch;
		uint8_t  Channel;
		uint8_t  Padding;
	} Data;
} REPLY_0702_t;

typedef struct {
	Header_t Header;
	uint8_t  On;
	uint8_t  Padding[3];
} CMD_0703_t;

typedef struct {
	Header_t Header;
	struct {
		uint8_t Result;
		uint8_t Ptt;
		uint8_t Function;
		uint8_t Padding;
	} Data;
} REPLY_0704_t;

typedef struct {
	Header_t Header;
	uint32_t Frequency;
} CMD_0705_t;

typedef struct {
	Header_t Header;
	struct {
		uint8_t  Result;
		uint8_t  Padding[3];
		uint32_t Frequency;
	} Data;
} REPLY_0706_t;

typedef struct {
	Header_t Header;
	struct {
		uint32_t Frequency;
		uint16_t RSSI;
		uint8_t  Noise;
		uint8_t  Glitch;
		uint8_t  Function;
		uint8_t  Vfo;
		uint8_t  Padding[2];
	} Data;
} EVENT_0710_t;

typedef struct {
	Header_t Header;
	struct {
		uint32_t Duration_10ms;
		uint16_t PeakRSSI;
		uint8_t  Padding[2];
	} Data;
} EVENT_0711_t;

// wire layout is fixed: tools/uvk5_cat.py depends on these sizes
static_assert(sizeof(REPLY_0702_t) == 4 + 20);
static_assert(sizeof(CMD_0703_t)   == 4 + 4);
static_assert(sizeof(REPLY_0704_t) == 4 + 4);
static_assert(sizeof(CMD_0705_t)   == 4 + 4);
static_assert(sizeof(REPLY_0706_t) == 4 + 8);
static_assert(sizeof(EVENT_0710_t) == 4 + 12);
static_assert(sizeof(EVENT_0711_t) == 4 + 8);

volatile bool gUartCatPtt;

static bool     catArmed;
static uint8_t  catPttTimer_10ms;
static bool     catRxActive;
static uint32_t catRxTime_10ms;
static uint16_t catRxPeakRssi;
static bool     catFreqPending;
static uint32_t catFreqRequest;

static uint16_t CAT_GetRssi(void)
{
	return BK4819_ReadRegister(BK4819_REG_67) & 0x01FF;
}

static void CMD_0701(void)
{
	REPLY_0702_t Reply;

	Reply.Header.ID          = 0x0702;
	Reply.Header.Size        = sizeof(Reply.Data);
	Reply.Data.RxFrequency   = gRxVfo->pRX->Frequency;
	Reply.Data.TxFrequency   = gTxVfo->pTX->Frequency;
	Reply.Data.RSSI          = CAT_GetRssi();
	Reply.Data.Battery_10mV  = gBatteryVoltageAverage;
	Reply.Data.Function      = gCurrentFunction;
	Reply.Data.RxVfo         = gEeprom.RX_VFO;
	Reply.Data.TxVfo         = gEeprom.TX_VFO;
	Reply.Data.Flags         = (FUNCTION_IsRx() ? 1u : 0u)
	                         | (gUartCatPtt     ? 2u : 0u)
	                         | (gPttIsPressed   ? 4u : 0u)
	                         | (catArmed        ? 8u : 0u);
	Reply.Data.Noise         = BK4819_ReadRegister(BK4819_REG_65) & 0x007F;
	Reply.Data.Glitch        = BK4819_ReadRegister(BK4819_REG_63) & 0x00FF;
	Reply.Data.Channel       = gTxVfo->CHANNEL_SAVE;
	Reply.Data.Padding       = 0;

	SendReply(&Reply, sizeof(Reply));
}

static void CAT_SendPttReply(uint8_t Result)
{
	REPLY_0704_t Reply;

	Reply.Header.ID     = 0x0704;
	Reply.Header.Size   = sizeof(Reply.Data);
	Reply.Data.Result   = Result;
	Reply.Data.Ptt      = gUartCatPtt;
	Reply.Data.Function = gCurrentFunction;
	Reply.Data.Padding  = 0;

	SendReply(&Reply, sizeof(Reply));
}

static void CMD_0703(const uint8_t *pBuffer)
{
	const CMD_0703_t *pCmd = (const CMD_0703_t *)pBuffer;

	if (pCmd->On && gReducedService) {
		CAT_SendPttReply(CAT_ERR_REJECTED);
		return;
	}

	gUartCatPtt      = pCmd->On != 0;
	catPttTimer_10ms = gUartCatPtt ? CAT_PTT_TIMEOUT_10ms : 0;
	CAT_SendPttReply(CAT_OK);
}

static void CAT_SendFreqReply(uint8_t Result, uint32_t Frequency)
{
	REPLY_0706_t Reply;

	memset(&Reply, 0, sizeof(Reply));
	Reply.Header.ID      = 0x0706;
	Reply.Header.Size    = sizeof(Reply.Data);
	Reply.Data.Result    = Result;
	Reply.Data.Frequency = Frequency;

	SendReply(&Reply, sizeof(Reply));
}

static void CMD_0705(const uint8_t *pBuffer)
{
	const CMD_0705_t *pCmd = (const CMD_0705_t *)pBuffer;

	// applied from UART_CAT_TimeSlice10ms(): EEPROM writes and register
	// reconfiguration must not run with interrupts disabled
	catFreqRequest = pCmd->Frequency;
	catFreqPending = true;
}

static uint8_t CAT_ApplyFrequency(uint32_t Frequency)
{
	if (gCurrentFunction == FUNCTION_TRANSMIT || gUartCatPtt || gPttIsPressed)
		return CAT_ERR_BUSY;

	if (!IS_FREQ_CHANNEL(gTxVfo->CHANNEL_SAVE))
		return CAT_ERR_NOT_VFO;

	if (RX_freq_check(Frequency) < 0)
		return CAT_ERR_RANGE;

	if (FREQUENCY_GetBand(Frequency) != gTxVfo->Band)
		return CAT_ERR_BAND;

	gTxVfo->freq_config_RX.Frequency = FREQUENCY_RoundToStep(Frequency, gTxVfo->StepFrequency);

	SETTINGS_SaveChannel(gTxVfo->CHANNEL_SAVE, gEeprom.TX_VFO, gTxVfo, 1);
	RADIO_ConfigureChannel(gEeprom.TX_VFO, VFO_CONFIGURE);
	RADIO_SelectVfos();
	RADIO_SetupRegisters(true);

	gUpdateDisplay = true;
	return CAT_OK;
}

static void CAT_SendRxStart(void)
{
	EVENT_0710_t Event;

	memset(&Event, 0, sizeof(Event));
	Event.Header.ID      = 0x0710;
	Event.Header.Size    = sizeof(Event.Data);
	Event.Data.Frequency = gRxVfo->pRX->Frequency;
	Event.Data.RSSI      = catRxPeakRssi;
	Event.Data.Noise     = BK4819_ReadRegister(BK4819_REG_65) & 0x007F;
	Event.Data.Glitch    = BK4819_ReadRegister(BK4819_REG_63) & 0x00FF;
	Event.Data.Function  = gCurrentFunction;
	Event.Data.Vfo       = gEeprom.RX_VFO;

	SendReply(&Event, sizeof(Event));
}

static void CAT_SendRxEnd(void)
{
	EVENT_0711_t Event;

	memset(&Event, 0, sizeof(Event));
	Event.Header.ID          = 0x0711;
	Event.Header.Size        = sizeof(Event.Data);
	Event.Data.Duration_10ms = catRxTime_10ms;
	Event.Data.PeakRSSI      = catRxPeakRssi;

	SendReply(&Event, sizeof(Event));
}

void UART_CAT_TimeSlice10ms(void)
{
	// PTT watchdog: a crashed or unplugged PC must not leave the radio keyed
	if (gUartCatPtt && --catPttTimer_10ms == 0)
		gUartCatPtt = false;

	if (catFreqPending) {
		catFreqPending = false;
		const uint8_t Result = CAT_ApplyFrequency(catFreqRequest);
		CAT_SendFreqReply(Result, gTxVfo->freq_config_RX.Frequency);
	}

	const bool rxNow = FUNCTION_IsRx();

	if (rxNow) {
		const uint16_t rssi = CAT_GetRssi();

		if (!catRxActive) {
			catRxActive    = true;
			catRxTime_10ms = 0;
			catRxPeakRssi  = rssi;
			if (catArmed)
				CAT_SendRxStart();
		}

		catRxTime_10ms++;
		if (rssi > catRxPeakRssi)
			catRxPeakRssi = rssi;
	}
	else if (catRxActive) {
		catRxActive = false;
		if (catArmed)
			CAT_SendRxEnd();
	}
}
#endif

#ifdef ENABLE_UART_RW_BK_REGS
static void CMD_0601_ReadBK4819Reg(const uint8_t *pBuffer)
{
	typedef struct  __attribute__((__packed__)) {
		Header_t header;
		uint8_t reg;
	} CMD_0601_t;

	CMD_0601_t *cmd = (CMD_0601_t*) pBuffer;

	struct __attribute__((__packed__)) {
		Header_t header;
		struct __attribute__((__packed__)) {
			uint8_t reg;
			uint16_t value;
		} data;
	} reply;

	reply.header.ID = 0x0601;
	reply.header.Size = sizeof(reply.data);
	reply.data.reg = cmd->reg;
	reply.data.value = BK4819_ReadRegister(cmd->reg);
	SendReply(&reply, sizeof(reply));
}

static void CMD_0602_WriteBK4819Reg(const uint8_t *pBuffer)
{
	typedef struct __attribute__((__packed__)) {
		Header_t header;
		uint8_t reg;
		uint16_t value;
	} CMD_0602_t;

	CMD_0602_t *cmd = (CMD_0602_t*) pBuffer;
	BK4819_WriteRegister(cmd->reg, cmd->value);
}
#endif

bool UART_IsCommandAvailable(void)
{
	uint16_t Index;
	uint16_t TailIndex;
	uint16_t Size;
	uint16_t CRC;
	uint16_t CommandLength;
	uint16_t DmaLength = DMA_CH0->ST & 0xFFFU;

	while (1)
	{
		if (gUART_WriteIndex == DmaLength)
			return false;

		while (gUART_WriteIndex != DmaLength && UART_DMA_Buffer[gUART_WriteIndex] != 0xABU)
			gUART_WriteIndex = DMA_INDEX(gUART_WriteIndex, 1);

		if (gUART_WriteIndex == DmaLength)
			return false;

		if (gUART_WriteIndex < DmaLength)
			CommandLength = DmaLength - gUART_WriteIndex;
		else
			CommandLength = (DmaLength + sizeof(UART_DMA_Buffer)) - gUART_WriteIndex;

		if (CommandLength < 8)
			return 0;

		if (UART_DMA_Buffer[DMA_INDEX(gUART_WriteIndex, 1)] == 0xCD)
			break;

		gUART_WriteIndex = DMA_INDEX(gUART_WriteIndex, 1);
	}

	Index = DMA_INDEX(gUART_WriteIndex, 2);
	Size  = (UART_DMA_Buffer[DMA_INDEX(Index, 1)] << 8) | UART_DMA_Buffer[Index];

	if ((Size + 8u) > sizeof(UART_DMA_Buffer))
	{
		gUART_WriteIndex = DmaLength;
		return false;
	}

	if (CommandLength < (Size + 8))
		return false;

	Index     = DMA_INDEX(Index, 2);
	TailIndex = DMA_INDEX(Index, Size + 2);

	if (UART_DMA_Buffer[TailIndex] != 0xDC || UART_DMA_Buffer[DMA_INDEX(TailIndex, 1)] != 0xBA)
	{
		gUART_WriteIndex = DmaLength;
		return false;
	}

	if (TailIndex < Index)
	{
		const uint16_t ChunkSize = sizeof(UART_DMA_Buffer) - Index;
		memcpy(UART_Command.Buffer, UART_DMA_Buffer + Index, ChunkSize);
		memcpy(UART_Command.Buffer + ChunkSize, UART_DMA_Buffer, TailIndex);
	}
	else
		memcpy(UART_Command.Buffer, UART_DMA_Buffer + Index, TailIndex - Index);

	TailIndex = DMA_INDEX(TailIndex, 2);
	if (TailIndex < gUART_WriteIndex)
	{
		memset(UART_DMA_Buffer + gUART_WriteIndex, 0, sizeof(UART_DMA_Buffer) - gUART_WriteIndex);
		memset(UART_DMA_Buffer, 0, TailIndex);
	}
	else
		memset(UART_DMA_Buffer + gUART_WriteIndex, 0, TailIndex - gUART_WriteIndex);

	gUART_WriteIndex = TailIndex;

	if (UART_Command.Header.ID == 0x0514)
		bIsEncrypted = false;

	if (UART_Command.Header.ID == 0x6902)
		bIsEncrypted = true;

	if (bIsEncrypted)
	{
		unsigned int i;
		for (i = 0; i < (Size + 2u); i++)
			UART_Command.Buffer[i] ^= Obfuscation[i % 16];
	}
	
	CRC = UART_Command.Buffer[Size] | (UART_Command.Buffer[Size + 1] << 8);

	return (CRC_Calculate(UART_Command.Buffer, Size) != CRC) ? false : true;
}

void UART_HandleCommand(void)
{
	switch (UART_Command.Header.ID)
	{
		case 0x0514:
#ifdef ENABLE_UART_CAT
			catArmed = false;   // programming session: no unsolicited packets
#endif
			CMD_0514(UART_Command.Buffer);
			break;
	
		case 0x051B:
			CMD_051B(UART_Command.Buffer);
			break;
	
		case 0x051D:
			CMD_051D(UART_Command.Buffer);
			break;
	
		case 0x051F:	// Not implementing non-authentic command
			break;
	
		case 0x0521:	// Not implementing non-authentic command
			break;
	
		case 0x0527:
			CMD_0527();
			break;
	
		case 0x0529:
			CMD_0529();
			break;
	
		case 0x052D:
			CMD_052D(UART_Command.Buffer);
			break;
	
		case 0x052F:
			CMD_052F(UART_Command.Buffer);
			break;
	
		case 0x05DD: // reset
			#if defined(ENABLE_OVERLAY)
				overlay_FLASH_RebootToBootloader();
			#else
				NVIC_SystemReset();
			#endif
			break;
			
#ifdef ENABLE_UART_CAT
		case 0x0701:
			catArmed = true;
			CMD_0701();
			break;

		case 0x0703:
			catArmed = true;
			CMD_0703(UART_Command.Buffer);
			break;

		case 0x0705:
			catArmed = true;
			CMD_0705(UART_Command.Buffer);
			break;
#endif

#ifdef ENABLE_UART_RW_BK_REGS
		case 0x0601:
			CMD_0601_ReadBK4819Reg(UART_Command.Buffer);
			break;
		
		case 0x0602:
			CMD_0602_WriteBK4819Reg(UART_Command.Buffer);
			break;
#endif
	}
}
