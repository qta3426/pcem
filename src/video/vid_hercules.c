/*Hercules emulation*/
#include <stdlib.h>
#include "ibm.h"
#include "device.h"
#include "io.h"
#include "mem.h"
#include "timer.h"
#include "video.h"
#include "vid_hercules.h"

typedef struct hercules_t {
	mem_mapping_t mapping;

	uint8_t crtc[32];
	int crtcreg;

	uint8_t ctrl, ctrl2, stat;

	uint64_t dispontime, dispofftime;
	pc_timer_t timer;

	int firstline, lastline;

	int linepos, displine;
	int vc, sc;
	uint16_t ma, maback;
	int con, coff, cursoron;
	int dispon, blink;
	int vsynctime, vadj;

	uint8_t *vram;
} hercules_t;

static uint32_t mdacols[256][2][2];

void hercules_recalctimings(hercules_t *hercules);
void hercules_write(uint32_t addr, uint8_t val, void *p);
uint8_t hercules_read(uint32_t addr, void *p);

void hercules_out(uint16_t addr, uint8_t val, void *p) {
	hercules_t *hercules = (hercules_t *)p;
//        pclog("Herc out %04X %02X\n",addr,val);
	switch (addr) {
	case 0x3b0:
	case 0x3b2:
	case 0x3b4:
	case 0x3b6:hercules->crtcreg = val & 31;
		return;
	case 0x3b1:
	case 0x3b3:
	case 0x3b5:
	case 0x3b7:hercules->crtc[hercules->crtcreg] = val;
		if (hercules->crtc[10] == 6 && hercules->crtc[11] == 7) /*Fix for Generic Turbo XT BIOS, which sets up cursor registers wrong*/
		{
			hercules->crtc[10] = 0xb;
			hercules->crtc[11] = 0xc;
		}
		hercules_recalctimings(hercules);
		return;
	case 0x3b8:hercules->ctrl = val;
		return;
	case 0x3bf:hercules->ctrl2 = val;
		if (val & 2)
			mem_mapping_set_addr(&hercules->mapping, 0xb0000, 0x10000);
		else
			mem_mapping_set_addr(&hercules->mapping, 0xb0000, 0x08000);
		return;
	}
}

uint8_t hercules_in(uint16_t addr, void *p) {
	hercules_t *hercules = (hercules_t *)p;
	//       pclog("Herc in %04X %02X %04X:%04X %04X\n",addr,(hercules_stat & 0xF) | ((hercules_stat & 8) << 4),CS,pc,CX);
	switch (addr) {
	case 0x3b0:
	case 0x3b2:
	case 0x3b4:
	case 0x3b6:return hercules->crtcreg;
	case 0x3b1:
	case 0x3b3:
	case 0x3b5:
	case 0x3b7:return hercules->crtc[hercules->crtcreg];
	case 0x3ba:return (hercules->stat & 0xf) | ((hercules->stat & 8) << 4);
	}
	return 0xff;
}

void hercules_write(uint32_t addr, uint8_t val, void *p) {
	hercules_t *hercules = (hercules_t *)p;
	egawrites++;
//        pclog("Herc write %08X %02X\n",addr,val);
	hercules->vram[addr & 0xffff] = val;
}

uint8_t hercules_read(uint32_t addr, void *p) {
	hercules_t *hercules = (hercules_t *)p;
	egareads++;
	return hercules->vram[addr & 0xffff];
}

void hercules_recalctimings(hercules_t *hercules) {
	double disptime;
	double _dispontime, _dispofftime;
	disptime = hercules->crtc[0] + 1;
	_dispontime = hercules->crtc[1];
	_dispofftime = disptime - _dispontime;
	_dispontime *= MDACONST;
	_dispofftime *= MDACONST;
	hercules->dispontime = (uint64_t)_dispontime;
	hercules->dispofftime = (uint64_t)_dispofftime;
}

void hercules_poll(void *p) {
	hercules_t *hercules = (hercules_t *)p;
	uint16_t ca = (hercules->crtc[15] | (hercules->crtc[14] << 8)) & 0x3fff;
	int drawcursor;
	int x, c;
	int oldvc;
	uint8_t chr, attr;
	uint16_t dat;
	int oldsc;
	int blink;
	if (!hercules->linepos) {
		//pclog("Poll %i %i\n",vc,sc);
		timer_advance_u64(&hercules->timer, hercules->dispofftime);
		hercules->stat |= 1;
		hercules->linepos = 1;
		oldsc = hercules->sc;
		if ((hercules->crtc[8] & 3) == 3)
			hercules->sc = (hercules->sc << 1) & 7;
		if (hercules->dispon) {
			if (hercules->displine < hercules->firstline) {
				hercules->firstline = hercules->displine;
				video_wait_for_buffer();
			}
			hercules->lastline = hercules->displine;
			if ((hercules->ctrl & 2) && (hercules->ctrl2 & 1)) {
				ca = (hercules->sc & 3) * 0x2000;
				if ((hercules->ctrl & 0x80) && (hercules->ctrl2 & 2))
					ca += 0x8000;
//                                printf("Draw herc %04X\n",ca);
				for (x = 0; x < hercules->crtc[1]; x++) {
					dat = (hercules->vram[((hercules->ma << 1) & 0x1fff) + ca] << 8) | hercules->vram[((hercules->ma << 1) & 0x1fff) + ca + 1];
					hercules->ma++;
					for (c = 0; c < 16; c++)
						((uint32_t *)buffer32->line[hercules->displine])[(x << 4) + c] = (dat & (32768 >> c)) ? cgapal[0x7] : 0;
				}
			} else {
				for (x = 0; x < hercules->crtc[1]; x++) {
					chr = hercules->vram[(hercules->ma << 1) & 0xfff];
					attr = hercules->vram[((hercules->ma << 1) + 1) & 0xfff];
					drawcursor = ((hercules->ma == ca) && hercules->con && hercules->cursoron);
					blink = ((hercules->blink & 16) && (hercules->ctrl & 0x20) && (attr & 0x80) && !drawcursor);
					if (hercules->sc == 12 && ((attr & 7) == 1)) {
						for (c = 0; c < 9; c++)
							((uint32_t *)buffer32->line[hercules->displine])[(x * 9) + c] = mdacols[attr][blink][1];
					} else {
						for (c = 0; c < 8; c++)
							((uint32_t *)buffer32->line[hercules->displine])[(x * 9) + c] = mdacols[attr][blink][(fontdatm[chr][hercules->sc] & (1 << (c ^ 7))) ? 1 : 0];
						if ((chr & ~0x1f) == 0xc0)
							((uint32_t *)buffer32->line[hercules->displine])[(x * 9) + 8] = mdacols[attr][blink][fontdatm[chr][hercules->sc] & 1];
						else
							((uint32_t *)buffer32->line[hercules->displine])[(x * 9) + 8] = mdacols[attr][blink][0];
					}
					hercules->ma++;
					if (drawcursor) {
						for (c = 0; c < 9; c++)
							((uint32_t *)buffer32->line[hercules->displine])[(x * 9) + c] ^= mdacols[attr][0][1];
					}
				}
			}
		}
		hercules->sc = oldsc;
		if (hercules->vc == hercules->crtc[7] && !hercules->sc) {
			hercules->stat |= 8;
//                        printf("VSYNC on %i %i\n",vc,sc);
		}
		hercules->displine++;
		if (hercules->displine >= 500)
			hercules->displine = 0;
	} else {
		timer_advance_u64(&hercules->timer, hercules->dispontime);
		if (hercules->dispon)
			hercules->stat &= ~1;
		hercules->linepos = 0;
		if (hercules->vsynctime) {
			hercules->vsynctime--;
			if (!hercules->vsynctime) {
				hercules->stat &= ~8;
//                                printf("VSYNC off %i %i\n",vc,sc);
			}
		}
		if (hercules->sc == (hercules->crtc[11] & 31) || ((hercules->crtc[8] & 3) == 3 && hercules->sc == ((hercules->crtc[11] & 31) >> 1))) {
			hercules->con = 0;
			hercules->coff = 1;
		}
		if (hercules->vadj) {
			hercules->sc++;
			hercules->sc &= 31;
			hercules->ma = hercules->maback;
			hercules->vadj--;
			if (!hercules->vadj) {
				hercules->dispon = 1;
				hercules->ma = hercules->maback = (hercules->crtc[13] | (hercules->crtc[12] << 8)) & 0x3fff;
				hercules->sc = 0;
			}
		} else if (hercules->sc == hercules->crtc[9] || ((hercules->crtc[8] & 3) == 3 && hercules->sc == (hercules->crtc[9] >> 1))) {
			hercules->maback = hercules->ma;
			hercules->sc = 0;
			oldvc = hercules->vc;
			hercules->vc++;
			hercules->vc &= 127;
			if (hercules->vc == hercules->crtc[6])
				hercules->dispon = 0;
			if (oldvc == hercules->crtc[4]) {
//                                printf("Display over at %i\n",displine);
				hercules->vc = 0;
				hercules->vadj = hercules->crtc[5];
				if (!hercules->vadj)
					hercules->dispon = 1;
				if (!hercules->vadj)
					hercules->ma = hercules->maback = (hercules->crtc[13] | (hercules->crtc[12] << 8)) & 0x3fff;
				if ((hercules->crtc[10] & 0x60) == 0x20)
					hercules->cursoron = 0;
				else
					hercules->cursoron = hercules->blink & 16;
			}
			if (hercules->vc == hercules->crtc[7]) {
				hercules->dispon = 0;
				hercules->displine = 0;
				hercules->vsynctime = 16;//(crtcm[3]>>4)+1;
				if (hercules->crtc[7]) {
//                                        printf("Lastline %i Firstline %i  %i\n",lastline,firstline,lastline-firstline);
					if ((hercules->ctrl & 2) && (hercules->ctrl2 & 1))
						x = hercules->crtc[1] << 4;
					else
						x = hercules->crtc[1] * 9;
					hercules->lastline++;
					if (x != xsize || (hercules->lastline - hercules->firstline) != ysize) {
						xsize = x;
						ysize = hercules->lastline - hercules->firstline;
//                                                printf("Resize to %i,%i - R1 %i\n",xsize,ysize,crtcm[1]);
						if (xsize < 64)
							xsize = 656;
						if (ysize < 32)
							ysize = 200;
						updatewindowsize(xsize, ysize);
					}

					video_blit_memtoscreen(0, hercules->firstline, 0, ysize, xsize, ysize);

					frames++;
					if ((hercules->ctrl & 2) && (hercules->ctrl2 & 1)) {
						video_res_x = hercules->crtc[1] * 16;
						video_res_y = hercules->crtc[6] * 4;
						video_bpp = 1;
					} else {
						video_res_x = hercules->crtc[1];
						video_res_y = hercules->crtc[6];
						video_bpp = 0;
					}
				}
				hercules->firstline = 1000;
				hercules->lastline = 0;
				hercules->blink++;
			}
		} else {
			hercules->sc++;
			hercules->sc &= 31;
			hercules->ma = hercules->maback;
		}
		if ((hercules->sc == (hercules->crtc[10] & 31) || ((hercules->crtc[8] & 3) == 3 && hercules->sc == ((hercules->crtc[10] & 31) >> 1)))) {
			hercules->con = 1;
//                        printf("Cursor on - %02X %02X %02X\n",crtcm[8],crtcm[10],crtcm[11]);
		}
	}
}

void *hercules_init() {
	int display_type;
	int c;
	hercules_t *hercules = malloc(sizeof(hercules_t));
	memset(hercules, 0, sizeof(hercules_t));

	hercules->vram = malloc(0x10000);

	timer_add(&hercules->timer, hercules_poll, hercules, 1);
	mem_mapping_add(&hercules->mapping, 0xb0000, 0x08000, hercules_read, NULL, NULL, hercules_write, NULL, NULL, NULL, MEM_MAPPING_EXTERNAL, hercules);
	io_sethandler(0x03b0, 0x0010, hercules_in, NULL, NULL, hercules_out, NULL, NULL, hercules);

	display_type = device_get_config_int("display_type");
	cgapal_rebuild(display_type, 0);

	for (c = 0; c < 256; c++) {
		mdacols[c][0][0] = mdacols[c][1][0] = mdacols[c][1][1] = cgapal[0];
		if (c & 8)
			mdacols[c][0][1] = cgapal[0xf];
		else
			mdacols[c][0][1] = cgapal[0x7];
	}
	mdacols[0x70][0][1] = cgapal[0];
	mdacols[0x70][0][0] = mdacols[0x70][1][0] = mdacols[0x70][1][1] = cgapal[0xf];
	mdacols[0xF0][0][1] = cgapal[0];
	mdacols[0xF0][0][0] = mdacols[0xF0][1][0] = mdacols[0xF0][1][1] = cgapal[0xf];
	mdacols[0x78][0][1] = cgapal[0x7];
	mdacols[0x78][0][0] = mdacols[0x78][1][0] = mdacols[0x78][1][1] = cgapal[0xf];
	mdacols[0xF8][0][1] = cgapal[0x7];
	mdacols[0xF8][0][0] = mdacols[0xF8][1][0] = mdacols[0xF8][1][1] = cgapal[0xf];
	mdacols[0x00][0][1] = mdacols[0x00][1][1] = cgapal[0];
	mdacols[0x08][0][1] = mdacols[0x08][1][1] = cgapal[0];
	mdacols[0x80][0][1] = mdacols[0x80][1][1] = cgapal[0];
	mdacols[0x88][0][1] = mdacols[0x88][1][1] = cgapal[0];

	return hercules;
}

void hercules_close(void *p) {
	hercules_t *hercules = (hercules_t *)p;

	free(hercules->vram);
	free(hercules);
}

void hercules_speed_changed(void *p) {
	hercules_t *hercules = (hercules_t *)p;

	hercules_recalctimings(hercules);
}

static device_config_t hercules_config[] =
	{
		{
			.name = "display_type",
			.description = "Display type",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "Green",
						.value = DISPLAY_GREEN
					},
					{
						.description = "Amber",
						.value = DISPLAY_AMBER
					},
					{
						.description = "White",
						.value = DISPLAY_WHITE
					},
					{
						.description = ""
					}
				},
			.default_int = DISPLAY_WHITE
		},
		{
			.type = -1
		}
	};

device_t hercules_device =
	{
		"Hercules",
		0,
		hercules_init,
		hercules_close,
		NULL,
		hercules_speed_changed,
		NULL,
		NULL,
		hercules_config
	};
