/*Oak OTI067 emulation*/
#include <stdlib.h>
#include "ibm.h"
#include "device.h"
#include "io.h"
#include "mem.h"
#include "rom.h"
#include "video.h"
#include "vid_oti067.h"
#include "vid_svga.h"
#include "acer386sx.h"

typedef struct oti067_t {
	svga_t svga;

	rom_t bios_rom;

	int index;
	uint8_t regs[32];

	uint8_t pos;
	uint8_t dipswitch_val;

	uint32_t vram_size;
	uint32_t vram_mask;
} oti067_t;

void oti067_out(uint16_t addr, uint8_t val, void *p) {
	oti067_t *oti067 = (oti067_t *)p;
	svga_t *svga = &oti067->svga;
	uint8_t old;

//        pclog("oti067_out : %04X %02X  %02X %i\n", addr, val, ram[0x489], ins);

	if ((((addr & 0xFFF0) == 0x3D0 || (addr & 0xFFF0) == 0x3B0) && addr < 0x3de) && !(svga->miscout & 1))
		addr ^= 0x60;

	switch (addr) {
	case 0x3D4:svga->crtcreg = val & 0x3f;
		return;
	case 0x3D5:
		if (svga->crtcreg & 0x20)
			return;
		if ((svga->crtcreg < 7) && (svga->crtc[0x11] & 0x80))
			return;
		if ((svga->crtcreg == 7) && (svga->crtc[0x11] & 0x80))
			val = (svga->crtc[7] & ~0x10) | (val & 0x10);
		old = svga->crtc[svga->crtcreg];
		svga->crtc[svga->crtcreg] = val;
		if (old != val) {
			if (svga->crtcreg < 0xE || svga->crtcreg > 0x10) {
				svga->fullchange = changeframecount;
				svga_recalctimings(svga);
			}
		}
		break;

	case 0x3DE:oti067->index = val & 0x1f;
		return;
	case 0x3DF:oti067->regs[oti067->index] = val;
		switch (oti067->index) {
		case 0xD:svga->vram_display_mask = (val & 0xc) ? oti067->vram_mask : 0x3ffff;
			if ((val & 0x80) && oti067->vram_size == 256)
				mem_mapping_disable(&svga->mapping);
			else
				mem_mapping_enable(&svga->mapping);
			if (!(val & 0x80))
				svga->vram_display_mask = 0x3ffff;
			break;
		case 0x11:svga->read_bank = (val & 0xf) * 65536;
			svga->write_bank = (val >> 4) * 65536;
			break;
		}
		return;
	}
	svga_out(addr, val, svga);
}

uint8_t oti067_in(uint16_t addr, void *p) {
	oti067_t *oti067 = (oti067_t *)p;
	svga_t *svga = &oti067->svga;
	uint8_t temp;

//        if (addr != 0x3da && addr != 0x3ba) pclog("oti067_in : %04X ", addr);

	if ((((addr & 0xFFF0) == 0x3D0 || (addr & 0xFFF0) == 0x3B0) && addr < 0x3de) && !(svga->miscout & 1))
		addr ^= 0x60;

	switch (addr) {
	case 0x3D4:temp = svga->crtcreg;
		break;
	case 0x3D5:
		if (svga->crtcreg & 0x20)
			temp = 0xff;
		else
			temp = svga->crtc[svga->crtcreg];
		break;

	case 0x3DE:temp = oti067->index | (2 << 5);
		break;
	case 0x3DF:
		if (oti067->index == 0x10)
			temp = oti067->dipswitch_val;
		else
			temp = oti067->regs[oti067->index];
		break;

	default:temp = svga_in(addr, svga);
		break;
	}
//        if (addr != 0x3da && addr != 0x3ba) pclog("%02X  %04X:%04X\n", temp, CS,pc);        
	return temp;
}

void oti067_pos_out(uint16_t addr, uint8_t val, void *p) {
	oti067_t *oti067 = (oti067_t *)p;

	if ((val & 8) != (oti067->pos & 8)) {
		if (val & 8)
			io_sethandler(0x03c0, 0x0020, oti067_in, NULL, NULL, oti067_out, NULL, NULL, oti067);
		else
			io_removehandler(0x03c0, 0x0020, oti067_in, NULL, NULL, oti067_out, NULL, NULL, oti067);
	}

	oti067->pos = val;
}

uint8_t oti067_pos_in(uint16_t addr, void *p) {
	oti067_t *oti067 = (oti067_t *)p;

	return oti067->pos;
}

void oti067_recalctimings(svga_t *svga) {
	oti067_t *oti067 = (oti067_t *)svga->p;

	if (oti067->regs[0x14] & 0x08)
		svga->ma_latch |= 0x10000;
	if (oti067->regs[0x0d] & 0x0c)
		svga->rowoffset <<= 1;
	svga->interlace = oti067->regs[0x14] & 0x80;
}

void *oti067_common_init(char *bios_fn, int vram_size) {
	oti067_t *oti067 = malloc(sizeof(oti067_t));
	memset(oti067, 0, sizeof(oti067_t));

	rom_init(&oti067->bios_rom, bios_fn, 0xc0000, 0x8000, 0x7fff, 0, MEM_MAPPING_EXTERNAL);

	oti067->vram_size = vram_size;
	oti067->vram_mask = (vram_size << 10) - 1;

	svga_init(&oti067->svga, oti067, vram_size << 10,
		  oti067_recalctimings,
		  oti067_in, oti067_out,
		  NULL,
		  NULL);

	io_sethandler(0x03c0, 0x0020, oti067_in, NULL, NULL, oti067_out, NULL, NULL, oti067);
	io_sethandler(0x46e8, 0x0001, oti067_pos_in, NULL, NULL, oti067_pos_out, NULL, NULL, oti067);

	oti067->svga.miscout = 1;

	oti067->dipswitch_val = 0x18;
	return oti067;
}

void oti067_enable_disable(void *p, int enable) {
	oti067_t *oti067 = (oti067_t *)p;

	mem_mapping_disable(&oti067->bios_rom.mapping);
	io_removehandler(0x03c0, 0x0020, oti067_in, NULL, NULL, oti067_out, NULL, NULL, oti067);
	io_removehandler(0x46e8, 0x0001, oti067_pos_in, NULL, NULL, oti067_pos_out, NULL, NULL, oti067);
	mem_mapping_disable(&oti067->svga.mapping);
	if (enable) {
		mem_mapping_enable(&oti067->bios_rom.mapping);
		io_sethandler(0x03c0, 0x0020, oti067_in, NULL, NULL, oti067_out, NULL, NULL, oti067);
		io_sethandler(0x46e8, 0x0001, oti067_pos_in, NULL, NULL, oti067_pos_out, NULL, NULL, oti067);
		mem_mapping_enable(&oti067->svga.mapping);
	}
}

void *oti067_init() {
	int vram_size = device_get_config_int("memory");
	return oti067_common_init("oti067/bios.bin", vram_size);
}

void *oti067_acer386_init() {
	oti067_t *oti067 = oti067_common_init("acer386/oti067.bin", 512);

	acer386sx_set_oti067(oti067);

	return oti067;
}

void *oti067_ama932j_init() {
	oti067_t *oti067 = oti067_common_init("ama932j/oti067.bin", 512);

	oti067->dipswitch_val |= 0x20;
	return oti067;
}

static int oti067_available() {
	return rom_present("oti067/bios.bin");
}

void oti067_close(void *p) {
	oti067_t *oti067 = (oti067_t *)p;

	svga_close(&oti067->svga);

	free(oti067);
}

void oti067_speed_changed(void *p) {
	oti067_t *oti067 = (oti067_t *)p;

	svga_recalctimings(&oti067->svga);
}

void oti067_force_redraw(void *p) {
	oti067_t *oti067 = (oti067_t *)p;

	oti067->svga.fullchange = changeframecount;
}

void oti067_add_status_info(char *s, int max_len, void *p) {
	oti067_t *oti067 = (oti067_t *)p;

	svga_add_status_info(s, max_len, &oti067->svga);
}

static device_config_t oti067_config[] =
	{
		{
			.name = "memory",
			.description = "Memory size",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "256 kB",
						.value = 256
					},
					{
						.description = "512 kB",
						.value = 512
					},
					{
						.description = ""
					}
				},
			.default_int = 512
		},
		{
			.type = -1
		}
	};

device_t oti067_device =
	{
		"Oak OTI-067",
		0,
		oti067_init,
		oti067_close,
		oti067_available,
		oti067_speed_changed,
		oti067_force_redraw,
		oti067_add_status_info,
		oti067_config
	};
device_t oti067_acer386_device =
	{
		"Oak OTI-067 (Acermate 386SX/25N)",
		0,
		oti067_acer386_init,
		oti067_close,
		oti067_available,
		oti067_speed_changed,
		oti067_force_redraw,
		oti067_add_status_info
	};
device_t oti067_ama932j_device =
	{
		"Oak OTI-067 (AMA-932J)",
		0,
		oti067_ama932j_init,
		oti067_close,
		oti067_available,
		oti067_speed_changed,
		oti067_force_redraw,
		oti067_add_status_info
	};
