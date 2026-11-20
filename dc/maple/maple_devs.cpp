#include "types.h"
#include "maple_if.h"
#include "maple_helper.h"
#include "maple_devs.h"
#include "maple_cfg.h"
#include <time.h>

const char* maple_sega_controller_name = "Dreamcast Controller";
const char* maple_sega_vmu_name = "Visual Memory";
const char* maple_sega_kbd_name = "Emulated Dreamcast Keyboard";
const char* maple_sega_mouse_name = "Emulated Dreamcast Mouse";
const char* maple_sega_dreameye_name_1 = "Dreamcast Camera Flash  Devic";
const char* maple_sega_dreameye_name_2 = "Dreamcast Camera Flash LDevic";
const char* maple_sega_mic_name = "MicDevice for Dreameye";
const char* maple_sega_purupuru_name = "Puru Puru Pack";

const char* maple_sega_brand = "Produced By or Under License From SEGA ENTERPRISES,LTD.";


enum MapleFunctionID
{
	MFID_0_Input		= 0x01000000,		//DC Controller, Lightgun buttons, arcade stick .. stuff like that
	MFID_1_Storage		= 0x02000000,		//VMU , VMS
	MFID_2_LCD			= 0x04000000,		//VMU
	MFID_3_Clock		= 0x08000000,		//VMU
	MFID_4_Mic			= 0x10000000,		//DC Mic (, dreameye too ?)
	MFID_5_ARGun		= 0x20000000,		//Artificial Retina gun ? seems like this one was never developed or smth -- i only remember of lightguns
	MFID_6_Keyboard		= 0x40000000,		//DC Keyboard
	MFID_7_LightGun		= 0x80000000,		//DC Lightgun
	MFID_8_Vibration	= 0x00010000,		//Puru Puru Puur~~~
	MFID_9_Mouse		= 0x00020000,		//DC Mouse
	MFID_10_StorageExt	= 0x00040000,		//Storage ? propably never used
	MFID_11_Camera		= 0x00080000,		//DreamEye
};

enum MapleDeviceCommand
{
	MDC_DeviceRequest	=0x01,			//7 words.Note : Initialises device
	MDC_AllStatusReq	=0x02,			//7 words + device depedant ( seems to be 8 words)
	MDC_DeviceReset		=0x03,			//0 words
	MDC_DeviceKill		=0x04,			//0 words
	MDC_DeviceStatus    =0x05,			//Same as MDC_DeviceRequest ?
	MDC_DeviceAllStatus =0x06,			//Same as MDC_AllStatusReq ?

	//Varius Functions
	MDCF_GetCondition=0x09,				//FT
	MDCF_GetMediaInfo=0x0A,				//FT,PT,3 pad
	MDCF_BlockRead   =0x0B,				//FT,PT,Phase,Block #
	MDCF_BlockWrite  =0x0C,				//FT,PT,Phase,Block #,data ...
	MDCF_GetLastError=0x0D,				//FT,PT,Phase,Block #
	MDCF_SetCondition=0x0E,				//FT,data ...
	MDCF_MICControl	 =0x0F,				//FT,MIC data ...
	MDCF_ARGunControl=0x10,				//FT,AR-Gun data ...
};

enum MapleDeviceRV
{
	MDRS_DeviceStatus=0x05,			//28 words
	MDRS_DeviceStatusAll=0x06,		//28 words + device depedant data
	MDRS_DeviceReply=0x07,			//0 words
	MDRS_DataTransfer=0x08,			//FT,depends on the command

	MDRE_UnknownFunction=0xFE,		//0 words
	MDRE_UnknownCmd=0xFD,			//0 words
	MDRE_TransminAgain=0xFC,		//0 words
	MDRE_FileError=0xFB,			//1 word, bitfield
	MDRE_LCDError=0xFA,				//1 word, bitfield
	MDRE_ARGunError=0xF9,			//1 word, bitfield
};



#define SWAP32(a) ((((a) & 0xff) << 24)  | (((a) & 0xff00) << 8) | (((a) >> 8) & 0xff00) | (((a) >> 24) & 0xff))

//fill in the info
void maple_device::Setup(u32 prt)
{
	maple_port = prt;
	bus_port = maple_GetPort(prt);
	bus_id = maple_GetBusId(prt);
	logical_port[0] = 'A' + bus_id;
	logical_port[1] = bus_port == 5 ? 'x' : '1' + bus_port;
	logical_port[2] = 0;
}
maple_device::~maple_device()
{
	if (config)
		delete config;
}

/*
	Base class with dma helpers and stuff
*/
struct maple_base: maple_device
{
	u8* dma_buffer_out;
	u32* dma_count_out;

	u8* dma_buffer_in;
	u32 dma_count_in;

	void w8(u8 data) { *host_ptr_xor((u8*)dma_buffer_out)=data;dma_buffer_out+=1;dma_count_out[0]+=1; }
	void w16(u16 data) { *host_ptr_xor((u16*)dma_buffer_out)=data;dma_buffer_out+=2;dma_count_out[0]+=2; }
	void w32(u32 data) { *(u32*)dma_buffer_out=data;dma_buffer_out+=4;dma_count_out[0]+=4; }

	void wptr(const void* src,u32 len)
	{
		u8* src8=(u8*)src;
		while(len--)
			w8(*src8++);
	}
	void wstr(const char* str,u32 len)
	{
		size_t ln=strlen(str);
		verify(len>=ln);
		len-=ln;
		while(ln--)
			w8(*str++);

		while(len--)
			w8(0x20);
	}
	
	u8 r8()	  { u8  rv=*host_ptr_xor((u8*)dma_buffer_in);dma_buffer_in+=1;dma_count_in-=1; return rv; }
	u16 r16() { u16 rv=*host_ptr_xor((u16*)dma_buffer_in);dma_buffer_in+=2;dma_count_in-=2; return rv; }
	u32 r32() { u32 rv=*(u32*)dma_buffer_in;dma_buffer_in+=4;dma_count_in-=4; return rv; }
	void rptr(const void* dst,u32 len)
	{
		u8* dst8=(u8*)dst;
		while(len--)
			*dst8++=r8();
	}
	u32 r_count() { return dma_count_in; }

	virtual u32 Dma(u32 Command,u32* buffer_in,u32 buffer_in_len,u32* buffer_out,u32& buffer_out_len)
	{
		dma_buffer_out=(u8*)buffer_out;
		dma_count_out=&buffer_out_len;

		dma_buffer_in=(u8*)buffer_in;
		dma_count_in=buffer_in_len;

		return dma(Command);
	}
	virtual u32 dma(u32 cmd)=0;
};

/*
	Sega Dreamcast Controller
	No error checking of anykind, but works just fine
*/
struct maple_sega_controller: maple_base
{
	virtual u32 dma(u32 cmd)
	{
		//printf("maple_sega_controller::dma Called 0x%X;Command %d\n",device_instance->port,Command);
		switch (cmd)
		{
		case MDC_DeviceRequest:
			//caps
			//4
			w32(MFID_0_Input);

			//struct data
			//3*4
			w32( 0xfe060f00);
			w32( 0);
			w32( 0);

			//1	area code
			w8(0xFF);

			//1	direction
			w8(0);
			
			//30
			wstr(maple_sega_controller_name,30);

			//60
			wstr(maple_sega_brand,60);

			//2
			w16(0x01AE); 

			//2
			w16(0x01F4);

			return MDRS_DeviceStatus;

			//controller condition
		case MDCF_GetCondition:
			{
				PlainJoystickState pjs;
				config->GetInput(&pjs);
				//caps
				//4
				w32(MFID_0_Input);

				//state data
				//2 key code
				w16(pjs.kcode);

				//trigers
				//1 R
				w8(pjs.trigger[PJTI_R]);
				//1 L
				w8(pjs.trigger[PJTI_L]);

				//joyx
				//1
				w8(pjs.joy[PJAI_X1]);
				//joyy
				//1
				w8(pjs.joy[PJAI_Y1]);

				//not used
				//1
				w8(0x80);
				//1
				w8(0x80);
			}

		return MDRS_DataTransfer;

		default:
			printf("UNKOWN MAPLE COMMAND %d\n",cmd);
			return MDRE_UnknownFunction;
		}
	}	
};

/*
	Sega Dreamcast Visual Memory Unit
	This is pretty much done (?)
*/
struct maple_sega_vmu: maple_base
{
	FILE* file;
#ifdef __WIIU__
	char path[512];   // the file is only open while it's read or written (see OnSetup)
#endif
	u8 flash_data[128*1024];
	u8 lcd_data[192];
	u8 lcd_data_decoded[48*32];
	
	// a freshly formatted card (mc.pp.se/dc/vms/flashmem.html): root block 255, FAT in 254, a 13-block
	// directory from 253 down to 241, 200 free user blocks; 16-bit fields little-endian
	void format_vmu()
	{
		memset(flash_data,0,sizeof(flash_data));
		u8* root=&flash_data[255*512];
		memset(root,0x55,16);
		const u8 bcd_time[8]={0x20,0x26,0x11,0x04,0x12,0x00,0x00,0x03};   // a fixed date; games only display it
		memcpy(root+0x30,bcd_time,8);
		auto le16=[](u8* at,u16 v){ at[0]=(u8)v; at[1]=(u8)(v>>8); };
		le16(root+0x46,254);   // FAT location
		le16(root+0x48,1);     // FAT size
		le16(root+0x4a,253);   // directory location
		le16(root+0x4c,13);    // directory size
		le16(root+0x4e,0);     // icon shape
		le16(root+0x50,200);   // user blocks
		u8* fat=&flash_data[254*512];
		for (u32 b=0;b<256;b++)
			le16(fat+b*2,0xfffc);                   // free
		for (u32 b=253;b>241;b--)
			le16(fat+b*2,(u16)(b-1));               // directory chain 253 -> 241
		le16(fat+241*2,0xfffa);
		le16(fat+254*2,0xfffa);                     // the FAT itself
		le16(fat+255*2,0xfffa);                     // the root block
	}

	virtual void OnSetup()
	{
		memset(flash_data,0,sizeof(flash_data));
		memset(lcd_data,0,sizeof(lcd_data));
		wchar temp[512];
#ifdef __WIIU__
		// next to the BIOS files: a bare name has no writable directory to land in on a Wii U
		{
			char name[64];
			sprintf(name,"data/vmu_save_%s.bin",logical_port);
			char* p=GetEmuPath(name);
			snprintf(path,sizeof(path),"%s",p);
			free(p);
		}
		// The devices get set up twice and the first set is never freed, so a file held open here made the
		// second open fail on a console (Cemu allows it), leaving empty 0-byte saves. Read it and close it;
		// a missing or short file becomes a full-size blank one, so each write can seek into it.
		file=0;
		FILE* f=fopen(path,"rb");
		const size_t got=f ? fread(flash_data,1,sizeof(flash_data),f) : 0;
		if (f) fclose(f);
		// an all-zero file is unformatted: format it (Soulcalibur reports no free space otherwise)
		bool blank=true;
		for (size_t i=0;i<sizeof(flash_data) && blank;i++)
			blank=flash_data[i]==0;
		if (got!=sizeof(flash_data) || blank)
		{
			printf("vmu save file \"%s\": %s, writing a freshly formatted one\n",path,!f ? "missing" : blank ? "blank" : "short");
			format_vmu();
			f=fopen(path,"wb");
			if (f)
			{
				fwrite(flash_data,1,sizeof(flash_data),f);
				fclose(f);
			}
			else
				printf("Failed to create vmu save file \"%s\"\n",path);
		}
#else
		sprintf(temp,"vmu_save_%s.bin",logical_port);
		file=fopen(temp,"rb+");
		if (!file)
		{
			printf("Unable to open vmu save file \"%s\", creating new file\n",temp);
			file=fopen(temp,"wb");
		}

		if (!file)
		{
			printf("Failed to create vmu save file \"%s\"\n",temp);
		}
		else
		{
			fread(flash_data,1,sizeof(flash_data),file);
		}
#endif
	}
	virtual ~maple_sega_vmu()
	{
		if (file) fclose(file);
	}
	virtual u32 dma(u32 cmd)
	{
		//printf("maple_sega_vmu::dma Called for port 0x%X, Command %d\n",device_instance->port,Command);
		switch (cmd)
		{
		case MDC_DeviceRequest:
			{
				//caps
				//4
				w32(MFID_1_Storage | MFID_2_LCD | MFID_3_Clock);

				//struct data
				//3*4
				w32( 0x403f7e7e);	//for clock
				w32( 0x00100500);	//for LCD
				w32( 0x00410f00);	//for storage
				//1	area code
				w8(0xFF);
				//1	direction
				w8(0);
				//30
				wstr(maple_sega_vmu_name,30);

				//60
				wstr(maple_sega_brand,60);

				//2
				w16(0x007c); 

				//2
				w16(0x0082); 

				return MDRS_DeviceStatus;
			}

			//in[0] is function used
			//out[0] is function used
		case MDCF_GetMediaInfo:
			{
				u32 function=r32();
				switch(function)
				{
				case MFID_1_Storage:
					{
						w32(MFID_1_Storage);
						
						//total_size;
						w16(0xff);
						//partition_number;
						w16(0);
						//system_area_block;
						w16(0xFF);
						//fat_area_block;
						w16(0xfe);
						//number_fat_areas_block;
						w16(1);
						//file_info_block;
						w16(0xfd);
						//number_info_blocks;
						w16(0xd);
						//volume_icon;
						w8(0);
						//reserved1;
						w8(0);
						//save_area_block;
						w16(0xc8);
						//number_of_save_blocks;
						w16(0x1f);
						//reserverd0 (something for execution files?)
						w32(0);

						return MDRS_DataTransfer;//data transfer
					}
					break;

				case MFID_2_LCD:
					{
						u32 pt=r32();
						if (pt!=0)
						{
							printf("VMU: MDCF_GetMediaInfo -> bad input |%08X|, returning MDRE_UnknownCmd\n",pt);
							return MDRE_UnknownCmd;
						}
						else
						{
							w32(MFID_2_LCD);

							w8(47);					//X dots -1
							w8(31);					//Y dots -1
							w8(((1)<<4) | (0));		//1 Color, 0 contrast levels
							w8(0);					//Padding
							
							return MDRS_DataTransfer;
						}
					}
					break;

				default:
					printf("VMU: MDCF_GetMediaInfo -> Bad function used |%08X|, returning -2\n",function);
					return MDRE_UnknownFunction;//bad function
				}
			}
			break;

		case MDCF_BlockRead:
			{
				u32 function=r32();
				switch(function)
				{
				case MFID_1_Storage:
					{
						w32(MFID_1_Storage);
						u32 xo=r32();
						u32 Block = (SWAP32(xo))&0xffff;
						w32(xo);

						if (Block>255)
						{
							printf("Block read : %d\n",Block);
							printf("BLOCK READ ERROR\n");
							Block&=255;
						}
						wptr(flash_data+Block*512,512);

						return MDRS_DataTransfer;//data transfer
					}
					break;

				case MFID_2_LCD:
					{
						w32(MFID_2_LCD);
						w32(r32()); // mnn ?
						wptr(flash_data,192);
						
						return MDRS_DataTransfer;//data transfer
					}
					break;

				case MFID_3_Clock:
					{
						if (r32()!=0)
						{
							printf("VMU: Block read: MFID_3_Clock : invalid params \n");
							return MDRE_TransminAgain;		//invalid params
						}
						else
						{
							w32(MFID_3_Clock);

							time_t now;
							time(&now);
							tm* timenow=localtime(&now);
							
							u8* timebuf=dma_buffer_out;

							w8((timenow->tm_year+1900)%256);
							w8((timenow->tm_year+1900)/256);

							w8(timenow->tm_mon+1);
							w8(timenow->tm_mday);

							w8(timenow->tm_hour);
							w8(timenow->tm_min);
							w8(timenow->tm_sec);
							w8(0);

							printf("VMU: CLOCK Read-> datetime is %04d/%02d/%02d ~ %02d:%02d:%02d!\n",timebuf[0]+timebuf[1]*256,timebuf[2],timebuf[3],timebuf[4],timebuf[5],timebuf[6]);

							return MDRS_DataTransfer;//transfer reply ...
						}
					}
					break;

				default:
					printf("VMU: cmd MDCF_BlockRead -> Bad function |%08X| used, returning -2\n",function);
					return MDRE_UnknownFunction;//bad function
				}
			}
			break;

		case MDCF_BlockWrite:
			{
				switch(r32())
				{
					case MFID_1_Storage:
					{
						u32 bph=r32();
						u32 Block = (SWAP32(bph))&0xffff;
						u32 Phase = ((SWAP32(bph))>>16)&0xff; 
						u32 write_adr=Block*512+Phase*(512/4);
						u32 write_len=r_count();
						rptr(&flash_data[write_adr],write_len);

#ifdef __WIIU__
						if (FILE* f=fopen(path,"rb+"))
						{
							fseek(f,write_adr,SEEK_SET);
							fwrite(&flash_data[write_adr],1,write_len,f);
							fclose(f);
						}
#else
						if (file)
						{
							fseek(file,write_adr,SEEK_SET);
							fwrite(&flash_data[write_adr],1,write_len,file);
							fflush(file);
						}
						else
						{
							printf("Failed to save vmu %s data\n",logical_port);
						}
#endif
						return MDRS_DeviceReply;//just ko
					}
					break;
					

					case MFID_2_LCD:
					{
						r32(); // skip unused word
						rptr(lcd_data,192);
						
						u8 white=0xff,black=0x00;

						for(int y=0;y<32;++y)
						{
							u8* dst=lcd_data_decoded+y*48;
							u8* src=lcd_data+6*y+5;
							for(int x=0;x<6;++x)
							{
								u8 col=*src--;
								for(int l=0;l<8;l++)
								{
									*dst++=col&1?black:white;
									col>>=1;
								}
							}
						}
						config->SetImage(lcd_data_decoded);
#if 0
						//Update lcd window
						if (!dev->lcd.visible)
						{
							dev->lcd.visible=true;
							ShowWindow(dev->lcd.handle,SHOW_OPENNOACTIVATE);
						}
						
							
							InvalidateRect(dev->lcd.handle,NULL,FALSE);
						}

						//Logitech G series stuff start
	#ifdef _HAS_LGLCD_
						{
							lgLcdBitmap160x43x1 bmp;
							bmp.hdr.Format = LGLCD_BMP_FORMAT_160x43x1;

							const BYTE white=0x00;
							const BYTE black=0xFF;

							//make it all black...
							memset(bmp.pixels,black,sizeof(bmp.pixels));

							//decode from the vmu
							for(int y=0;y<32;++y)
							{
								BYTE *dst=bmp.pixels+5816+((-y)*(48+112)); //ugly way to make things look right :p
								BYTE *src=dev->lcd.data+6*y+5;
								for(int x=0;x<48/8;++x)
								{
									BYTE val=*src;
									for(int m=0;m<8;++m)
									{
										if(val&(1<<(m)))
											*dst++=black;
										else
											*dst++=white;
									}
									--src;
								}
							}

							//Set the damned bits
							res = lgLcdUpdateBitmap(openContext.device, &bmp.hdr, LGLCD_ASYNC_UPDATE(LGLCD_PRIORITY_NORMAL));
						}
	#endif
						//Logitech G series stuff end
#endif
						return  MDRS_DeviceReply;//just ko
					}
					break;

					case MFID_3_Clock:
					{
						if (r32()!=0 || r_count()!=8)
							return MDRE_TransminAgain;	//invalid params ...
						else
						{
							u8 timebuf[8];
							rptr(timebuf,8);
							printf("VMU: CLOCK Write-> datetime is %04d/%02d/%02d ~ %02d:%02d:%02d! Nothing set tho ...\n",timebuf[0]+timebuf[1]*256,timebuf[2],timebuf[3],timebuf[4],timebuf[5],timebuf[6]);
							return  MDRS_DeviceReply;//ok !
						}
					}
					break;

					default:
					{
						printf("VMU: cmd MDCF_BlockWrite -> Bad function used, returning MDRE_UnknownFunction\n");
						return  MDRE_UnknownFunction;//bad function
					}
				}
			}
			break;

		case MDCF_GetLastError:
			return MDRS_DeviceReply;//just ko

		case MDCF_SetCondition:
			{
				switch(r32())
				{
				case MFID_3_Clock:
					{
						u32 bp=r32();
						if (bp)
						{
							printf("BEEP : %08X\n",bp);
						}
						return  MDRS_DeviceReply;//just ko
					}
					break;

				default:
					{
						printf("VMU: cmd MDCF_SetCondition -> Bad function used, returning MDRE_UnknownFunction\n");
						return MDRE_UnknownFunction;//bad function
					}
					break;
				}
			}


		default:
			printf("unknown MAPLE COMMAND %d\n",cmd);
			return MDRE_UnknownCmd;
		}
	}	
};

// ============================================================================
// External update functions defined in the special device .cpp files.
// Called once per Maple poll (GetCondition) per active port.
// ============================================================================
void UpdateLightGunState(u32 port);
void UpdateMaracasState(u32 port);
void UpdateKeyboardState(u32 port);
void UpdateFishingRodState(u32 port);

// Exposed from drkMapleLightGun.cpp for coordinate reporting
extern "C" u16 GetLightGunX(int port);
extern "C" u16 GetLightGunY(int port);

// DC_KeyboardReport layout (mirrors USB HID boot protocol, same as DC Maple).
// The full definition lives in drkMapleKeyboard.cpp; we redeclare it here so
// maple_keyboard::dma() can read the fields without pulling in that header.
struct DC_KeyboardReport
{
	u8 modifiers;
	u8 leds;
	u8 keys[6];
};

// Exposed from drkMapleKeyboard.cpp for key-code reporting
extern "C" const DC_KeyboardReport* GetKeyboardReport(int port);

// ============================================================================
// Light Gun (Dreamcast "Stunner" / gun peripheral)
//
// Real hardware notes (from Maple bus captures):
//   MDC_DeviceRequest function word: MFID_7_LightGun only (NOT combined with
//   MFID_0_Input — that combination is not a real DC device and confuses BIOS).
//
//   MDCF_GetCondition response layout (10 bytes after the function word):
//     u32  function   = MFID_7_LightGun
//     u16  buttons    (active-low, same bit layout as standard controller)
//     u16  gun_x      (0-639 NTSC; 0xFFFF = off-screen / missed)
//     u16  gun_y      (0-479 NTSC; 0xFFFF = off-screen / missed)
//   Total data words: 3 (12 bytes, last 2 bytes are the two u16 coords packed
//   after the button u16 — all naturally aligned).
//
//   MDC_AllStatusReq / MDC_DeviceStatus: BIOS probes these during enumeration.
//   We respond the same as MDC_DeviceRequest to avoid a hang.
// ============================================================================
const char* maple_sega_lightgun_name = "Dreamcast Gun";

// The gun, as Flycast does it (works with The House of the Dead 2): it answers as a controller (MFID_0_Input with
// the gun function), and its position isn't in that answer. In light-gun mode (maple DMA pattern 2) the PVR latches
// the beam position into SPG_TRIGGER_POS at the line the gun sees, and raises the maple V-blank-over interrupt.
void spg_lightgun_latch(int x, int y);   // SPG.cpp

struct maple_lightgun : maple_base
{
	virtual u32 dma(u32 cmd)
	{
		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_DeviceStatus:
		case MDC_AllStatusReq:
			w32(MFID_7_LightGun | MFID_0_Input);
			w32(0);            // light gun
			w32(0xFE000000);   // controller: A, B, Start, D-pad
			w32(0);
			w8(0x01);          // area: North America
			w8(0);             // direction
			wstr(maple_sega_lightgun_name, 30);
			wstr(maple_sega_brand, 60);
			w16(0x0069);       // standby current, 10.5 mA
			w16(0x0120);       // maximum, 28.8 mA
			return MDRS_DeviceStatus;

		case MDC_DeviceReset:
		case MDC_DeviceKill:
			return MDRS_DeviceReply;

		case MDCF_GetCondition:
			{
				PlainJoystickState pjs;
				config->GetInput(&pjs);   // -> UpdateLightGunState()
				w32(MFID_0_Input);
				w16(pjs.kcode | 0xFF01);  // the gun has A (trigger), B, Start and the D-pad
				w16(0xFFFF);
				w32(0x80808080);
			}
			return MDRS_DataTransfer;

		default:
			return MDRE_UnknownFunction;
		}
	}

	virtual bool get_lightgun_pos()
	{
		const u16 x = GetLightGunX(bus_id), y = GetLightGunY(bus_id);
		spg_lightgun_latch(x == 0xFFFF ? -1 : x, y == 0xFFFF ? -1 : y);
		return true;
	}
};

// ============================================================================
// Maracas (Samba de Amigo)
// Maple function: MFID_0_Input
// Height encoded in joyx/joyy; shake in lt/rt.
// ============================================================================
const char* maple_sega_maracas_name = "Maracas Controller";

struct maple_maracas : maple_base
{
	virtual u32 dma(u32 cmd)
	{
		switch (cmd)
		{
		case MDC_DeviceRequest:
			w32(MFID_0_Input);

			w32(0xfe060f00);
			w32(0);
			w32(0);

			w8(0xFF);
			w8(0);

			wstr(maple_sega_maracas_name, 30);
			wstr(maple_sega_brand, 60);

			w16(0x01AE);
			w16(0x01F4);

			return MDRS_DeviceStatus;

		case MDCF_GetCondition:
			{
				PlainJoystickState pjs;
				config->GetInput(&pjs);   // calls UpdateMaracasState via MapleConfigMap

				w32(MFID_0_Input);

				// Button state
				w16(pjs.kcode);

				// Shake on trigger bytes (255 = shaking)
				w8(pjs.trigger[PJTI_R]);  // right maraca shake
				w8(pjs.trigger[PJTI_L]);  // left  maraca shake

				// Height on analog axes (centered at 0x80)
				w8(pjs.joy[PJAI_X1]);     // left  maraca height
				w8(pjs.joy[PJAI_Y1]);     // right maraca height

				// Unused analog axes (centered)
				w8(0x80);
				w8(0x80);
			}
			return MDRS_DataTransfer;

		default:
			printf("maple_maracas: unknown command %d\n", cmd);
			return MDRE_UnknownFunction;
		}
	}
};

// ============================================================================
// Keyboard (Typing of the Dead)
// Maple function: MFID_6_Keyboard
// Reports USB HID key codes directly — DC keyboard IS a USB HID keyboard.
// ============================================================================
const char* maple_sega_keyboard_name_local = "Emulated Dreamcast Keyboard";

struct maple_keyboard : maple_base
{
	virtual u32 dma(u32 cmd)
	{
		switch (cmd)
		{
		case MDC_DeviceRequest:
			w32(MFID_6_Keyboard);

			// Keyboard capability word (matches real DC keyboard)
			w32(0x00000002);  // 2-byte LED + 6-byte key array
			w32(0);
			w32(0);

			w8(0xFF);
			w8(0);

			wstr(maple_sega_keyboard_name_local, 30);
			wstr(maple_sega_brand, 60);

			w16(0x0110);
			w16(0x0200);

			return MDRS_DeviceStatus;

		case MDCF_GetCondition:
			{
				// Trigger the platform update so key state is fresh
				config->GetInput(nullptr);

				int port = bus_id;
				const DC_KeyboardReport* kr = GetKeyboardReport(port);

				w32(MFID_6_Keyboard);

				if (kr)
				{
					w8(kr->modifiers);
					w8(kr->leds);
					for (int i = 0; i < 6; i++)
						w8(kr->keys[i]);
				}
				else
				{
					// No report available — send all-keys-up
					w8(0); // modifiers
					w8(0); // leds
					for (int i = 0; i < 6; i++)
						w8(0);
				}
			}
			return MDRS_DataTransfer;

		default:
			printf("maple_keyboard: unknown command %d\n", cmd);
			return MDRE_UnknownFunction;
		}
	}
};

// ============================================================================
// Fishing Rod (Sega Bass Fishing)
// Maple function: MFID_0_Input
// Cast state on kcode bit B, reel speed on rt, rod direction on joyx/joyy,
// cast tension on lt.
// ============================================================================
const char* maple_sega_fishingrod_name = "Fishing Controller";

struct maple_fishing_rod : maple_base
{
	virtual u32 dma(u32 cmd)
	{
		switch (cmd)
		{
		case MDC_DeviceRequest:
			w32(MFID_0_Input);

			w32(0xfe060f00);
			w32(0);
			w32(0);

			w8(0xFF);
			w8(0);

			wstr(maple_sega_fishingrod_name, 30);
			wstr(maple_sega_brand, 60);

			w16(0x01AE);
			w16(0x01F4);

			return MDRS_DeviceStatus;

		case MDCF_GetCondition:
			{
				PlainJoystickState pjs;
				config->GetInput(&pjs);   // calls UpdateFishingRodState via MapleConfigMap

				w32(MFID_0_Input);

				// Button state (cast = B bit, etc.)
				w16(pjs.kcode);

				// rt = reel speed (0-255), lt = cast tension (0-255)
				w8(pjs.trigger[PJTI_R]);  // reel speed
				w8(pjs.trigger[PJTI_L]);  // cast tension

				// Rod tip direction
				w8(pjs.joy[PJAI_X1]);     // horizontal
				w8(pjs.joy[PJAI_Y1]);     // vertical

				// Unused
				w8(0x80);
				w8(0x80);
			}
			return MDRS_DataTransfer;

		default:
			printf("maple_fishing_rod: unknown command %d\n", cmd);
			return MDRE_UnknownFunction;
		}
	}
};

// ============================================================================
// Puru Puru Pack: the rumble pack, as Flycast's maple_sega_purupuru (core/hw/maple/maple_devs.cpp). The game sets
// a vibration (power, frequency, a ramp) and an auto-stop time; wiiu/rumble_wiiu.cpp turns that into the bus's
// player's motors for the computed time. The ramp ("inclination") isn't played: the motors are on or off
// ============================================================================
extern "C" void wiiu_rumble_set(int bus, float power, u32 ms);   // wiiu/rumble_wiiu.cpp
struct maple_sega_purupuru : maple_base
{
	u16 AST = 19, AST_ms = 5000;
	u32 VIBSET = 0;

	u32 dma(u32 cmd)
	{
		static int logged;
		if (cmd != MDC_DeviceRequest && logged++ < 12)
			printf("[rumble] pack %s: command %u\n", logical_port, cmd);   // the game uses the pack
		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_AllStatusReq:
			w32(MFID_8_Vibration);
			w32(0x00000101);
			w32(0);
			w32(0);
			w8(0xFF);   // area code
			w8(0);      // direction
			wstr(maple_sega_purupuru_name, 30);
			wstr(maple_sega_brand, 60);
			w16(0x00C8);   // 20 mA standby
			w16(0x0640);   // 160 mA max
			if (cmd == MDC_AllStatusReq)
			{
				const char *extra = "Version 1.000,1998/11/10,315-6211-AH   ,Vibration Motor:1,Fm:4 - 30Hz,Pow:7     ";
				wptr(extra, strlen(extra));
				return MDRS_DeviceStatusAll;
			}
			return MDRS_DeviceStatus;

		case MDCF_GetCondition:
			w32(MFID_8_Vibration);
			w32(VIBSET);
			return MDRS_DataTransfer;

		case MDCF_GetMediaInfo:
			w32(MFID_8_Vibration);
			w32(0x3B07E010);   // the pack's vibration specs
			return MDRS_DataTransfer;

		case MDCF_BlockRead:
			w32(MFID_8_Vibration);
			w32(0);
			w16(2);
			w16(AST);
			return MDRS_DataTransfer;

		case MDCF_BlockWrite:
			{
				// the auto-stop time: byte 10 of the little-endian message, bits 16-23 of the third word here
				r32();
				r32();
				const u32 d = r32();
				AST = (d >> 16) & 0xFF;
				AST_ms = AST * 250 + 250;
			}
			return MDRS_DeviceReply;

		case MDCF_SetCondition:
			{
				r32();   // the function
				VIBSET = r32();
				const int pos = (VIBSET >> 8) & 7, neg = (VIBSET >> 12) & 7, freq = (VIBSET >> 16) & 0xFF;
				int inc = (VIBSET >> 24) & 0xFF;
				if (VIBSET & 0x8000)
					inc = -inc;              // INH: ramping down
				else if (!(VIBSET & 0x0800))
					inc = 0;                 // no EXH: no ramp
				const bool cnt = VIBSET & 1;
				const float power = pos + neg >= 7 ? 1.0f : (pos + neg) / 7.0f;
				u32 ms = AST_ms;
				if (freq > 0 && (!cnt || inc))
				{
					const int big = pos > neg ? pos : neg;
					const int t = 1000 * (inc ? (inc < 0 ? -inc : inc) * big : 1) / freq;
					ms = t < (int)AST_ms ? (u32)t : AST_ms;
				}
				wiiu_rumble_set(bus_id, power, ms);
			}
			return MDRS_DeviceReply;

		case MDC_DeviceReset:
		case MDC_DeviceKill:
			return MDRS_DeviceReply;

		default:
			return MDRE_UnknownCmd;
		}
	}
};

maple_device* maple_Create(MapleDeviceType type)
{
	maple_device* rv=0;
	switch(type)
	{
	case MDT_SegaController:
		rv=new maple_sega_controller();
		break;

	case MDT_SegaVMU:
		rv = new maple_sega_vmu();
		break;

	case MDT_LightGun:
		rv = new maple_lightgun();
		break;

	case MDT_Maracas:
		rv = new maple_maracas();
		break;

	case MDT_Keyboard:
		rv = new maple_keyboard();
		break;

	case MDT_FishingRod:
		rv = new maple_fishing_rod();
		break;

	case MDT_PurupuruPack:
		rv = new maple_sega_purupuru();
		break;

	default:
		return 0;
	}

	return rv;
}