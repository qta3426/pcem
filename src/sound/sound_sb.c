#include <stdlib.h>
#include "ibm.h"
#include "device.h"
#include "io.h"
#include "mca.h"
#include "mem.h"
#include "rom.h"
#include "sound.h"
#include "sound_emu8k.h"
#include "sound_mpu401_uart.h"
#include "sound_opl.h"
#include "sound_sb.h"
#include "sound_sb_dsp.h"

#include "filters.h"

//#define SB_DSP_RECORD_DEBUG

#ifdef SB_DSP_RECORD_DEBUG
FILE* soundfsb = 0/*NULL*/;
FILE* soundfsbin = 0/*NULL*/;
#endif

/* 0 to 7 -> -14dB to 0dB i 2dB steps. 8 to 15 -> 0 to +14dB in 2dB steps.
  Note that for positive dB values, this is not amplitude, it is amplitude-1. */
const float sb_bass_treble_4bits[] = {
	0.199526231, 0.25, 0.316227766, 0.398107170, 0.5, 0.63095734, 0.794328234, 1,
	0, 0.25892541, 0.584893192, 1, 1.511886431, 2.16227766, 3, 4.011872336
};

/* Attenuation tables for the mixer. Max volume = 32767 in order to give 6dB of 
 * headroom and avoid integer overflow */
const int32_t sb_att_2dbstep_5bits[] =
	{
		25, 32, 41, 51, 65, 82, 103, 130, 164, 206, 260, 327, 412, 519, 653,
		822, 1036, 1304, 1641, 2067, 2602, 3276, 4125, 5192, 6537, 8230, 10362, 13044,
		16422, 20674, 26027, 32767
	};
const int32_t sb_att_4dbstep_3bits[] =
	{
		164, 2067, 3276, 5193, 8230, 13045, 20675, 32767
	};
const int32_t sb_att_7dbstep_2bits[] =
	{
		164, 6537, 14637, 32767
	};

/* sb 1, 1.5, 2, 2 mvc do not have a mixer, so signal is hardwired */
static void sb_get_buffer_sb2(int32_t *buffer, int len, void *p) {
	sb_t *sb = (sb_t *)p;

	int c;

	opl2_update2(&sb->opl);
	sb_dsp_update(&sb->dsp);
	for (c = 0; c < len * 2; c += 2) {
		int32_t out;
		out = ((sb->opl.buffer[c] * 51000) >> 16);
		//TODO: Recording: Mic and line In with AGC
		out += (int32_t)(((sb_iir(0, (float)sb->dsp.buffer[c]) / 1.3) * 65536) / 3) >> 16;

		buffer[c] += out;
		buffer[c + 1] += out;
	}

	sb->pos = 0;
	sb->opl.pos = 0;
	sb->dsp.pos = 0;
}

static void sb_get_buffer_sb2_mixer(int32_t *buffer, int len, void *p) {
	sb_t *sb = (sb_t *)p;
	sb_ct1335_mixer_t *mixer = &sb->mixer_sb2;

	int c;

	opl2_update2(&sb->opl);
	sb_dsp_update(&sb->dsp);
	for (c = 0; c < len * 2; c += 2) {
		int32_t out;

		out = ((((sb->opl.buffer[c] * mixer->fm) >> 16) * 51000) >> 15);
		/* TODO: Recording : I assume it has direct mic and line in like sb2 */
		/* It is unclear from the docs if it has a filter, but it probably does */
		out += (int32_t)(((sb_iir(0, (float)sb->dsp.buffer[c]) / 1.3) * mixer->voice) / 3) >> 15;

		out = (out * mixer->master) >> 15;

		buffer[c] += out;
		buffer[c + 1] += out;
	}

	sb->pos = 0;
	sb->opl.pos = 0;
	sb->dsp.pos = 0;
}

void sb_get_buffer_sbpro(int32_t *buffer, int len, void *p) {
	sb_t *sb = (sb_t *)p;
	sb_ct1345_mixer_t *mixer = &sb->mixer_sbpro;

	int c;

	if (sb->dsp.sb_type == SBPRO)
		opl2_update2(&sb->opl);
	else
		opl3_update2(&sb->opl);

	sb_dsp_update(&sb->dsp);
	for (c = 0; c < len * 2; c += 2) {
		int32_t out_l, out_r;

		out_l = ((((sb->opl.buffer[c] * mixer->fm_l) >> 16) * (sb->opl_emu ? 47000 : 51000)) >> 15);
		out_r = ((((sb->opl.buffer[c + 1] * mixer->fm_r) >> 16) * (sb->opl_emu ? 47000 : 51000)) >> 15);

		/*TODO: Implement the stereo switch on the mixer instead of on the dsp? */
		if (mixer->output_filter) {
			out_l += (int32_t)(((sb_iir(0, (float)sb->dsp.buffer[c]) / 1.3) * mixer->voice_l) / 3) >> 15;
			out_r += (int32_t)(((sb_iir(1, (float)sb->dsp.buffer[c + 1]) / 1.3) * mixer->voice_r) / 3) >> 15;
		} else {
			out_l += ((int32_t)(sb->dsp.buffer[c] * mixer->voice_l) / 3) >> 15;
			out_r += ((int32_t)(sb->dsp.buffer[c + 1] * mixer->voice_r) / 3) >> 15;
		}
		//TODO: recording CD, Mic with AGC or line in. Note: mic volume does not affect recording.

		out_l = (out_l * mixer->master_l) >> 15;
		out_r = (out_r * mixer->master_r) >> 15;

		buffer[c] += out_l;
		buffer[c + 1] += out_r;
	}

	sb->pos = 0;
	sb->opl.pos = 0;
	sb->dsp.pos = 0;
}

static void sb_get_buffer_sb16(int32_t *buffer, int len, void *p) {
	sb_t *sb = (sb_t *)p;
	sb_ct1745_mixer_t *mixer = &sb->mixer_sb16;

	int c;

	opl3_update2(&sb->opl);
	sb_dsp_update(&sb->dsp);
	const int dsp_rec_pos = sb->dsp.record_pos_write;
	for (c = 0; c < len * 2; c += 2) {
		int32_t out_l, out_r, in_l, in_r;

		out_l = ((((sb->opl.buffer[c] * mixer->fm_l) >> 16) * (sb->opl_emu ? 47000 : 51000)) >> 15);
		out_r = ((((sb->opl.buffer[c + 1] * mixer->fm_r) >> 16) * (sb->opl_emu ? 47000 : 51000)) >> 15);

		/*TODO: multi-recording mic with agc/+20db, cd and line in with channel inversion */
		in_l = (mixer->input_selector_left & INPUT_MIDI_L) ? out_l : 0 + (mixer->input_selector_left & INPUT_MIDI_R) ? out_r : 0;
		in_r = (mixer->input_selector_right & INPUT_MIDI_L) ? out_l : 0 + (mixer->input_selector_right & INPUT_MIDI_R) ? out_r : 0;

		out_l += ((int32_t)(low_fir_sb16(0, (float)sb->dsp.buffer[c]) * mixer->voice_l) / 3) >> 15;
		out_r += ((int32_t)(low_fir_sb16(1, (float)sb->dsp.buffer[c + 1]) * mixer->voice_r) / 3) >> 15;

		out_l = (out_l * mixer->master_l) >> 15;
		out_r = (out_r * mixer->master_r) >> 15;

		if (mixer->bass_l != 8 || mixer->bass_r != 8 || mixer->treble_l != 8 || mixer->treble_r != 8) {
			/* This is not exactly how one does bass/treble controls, but the end result is like it. A better implementation would reduce the cpu usage */
			if (mixer->bass_l > 8)
				out_l += (int32_t)(low_iir(0, (float)out_l) * sb_bass_treble_4bits[mixer->bass_l]);
			if (mixer->bass_r > 8)
				out_r += (int32_t)(low_iir(1, (float)out_r) * sb_bass_treble_4bits[mixer->bass_r]);
			if (mixer->treble_l > 8)
				out_l += (int32_t)(high_iir(0, (float)out_l) * sb_bass_treble_4bits[mixer->treble_l]);
			if (mixer->treble_r > 8)
				out_r += (int32_t)(high_iir(1, (float)out_r) * sb_bass_treble_4bits[mixer->treble_r]);
			if (mixer->bass_l < 8)
				out_l = (int32_t)((out_l) * sb_bass_treble_4bits[mixer->bass_l] + low_cut_iir(0, (float)out_l) * (1.f - sb_bass_treble_4bits[mixer->bass_l]));
			if (mixer->bass_r < 8)
				out_r = (int32_t)((out_r) * sb_bass_treble_4bits[mixer->bass_r] + low_cut_iir(1, (float)out_r) * (1.f - sb_bass_treble_4bits[mixer->bass_r]));
			if (mixer->treble_l < 8)
				out_l = (int32_t)((out_l) * sb_bass_treble_4bits[mixer->treble_l] + high_cut_iir(0, (float)out_l) * (1.f - sb_bass_treble_4bits[mixer->treble_l]));
			if (mixer->treble_r < 8)
				out_r = (int32_t)((out_r) * sb_bass_treble_4bits[mixer->treble_r] + high_cut_iir(1, (float)out_r) * (1.f - sb_bass_treble_4bits[mixer->treble_r]));
		}
		if (sb->dsp.sb_enable_i) {
			int c_record = dsp_rec_pos;
			c_record += (((c / 2) * sb->dsp.sb_freq) / 48000) * 2;
			in_l <<= mixer->input_gain_L;
			in_r <<= mixer->input_gain_R;
			// Clip signal
			if (in_l < -32768)
				in_l = -32768;
			else if (in_l > 32767)
				in_l = 32767;

			if (in_r < -32768)
				in_r = -32768;
			else if (in_r > 32767)
				in_r = 32767;
			sb->dsp.record_buffer[c_record & 0xFFFF] = in_l;
			sb->dsp.record_buffer[(c_record + 1) & 0xFFFF] = in_r;
		}

		buffer[c] += (out_l << mixer->output_gain_L);
		buffer[c + 1] += (out_r << mixer->output_gain_R);
	}
	sb->dsp.record_pos_write += ((len * sb->dsp.sb_freq) / 48000) * 2;
	sb->dsp.record_pos_write &= 0xFFFF;

	sb->pos = 0;
	sb->opl.pos = 0;
	sb->dsp.pos = 0;
}
#ifdef SB_DSP_RECORD_DEBUG
int old_dsp_rec_pos=0;
int buf_written=0;
int last_crecord=0;
#endif
static void sb_get_buffer_emu8k(int32_t *buffer, int len, void *p) {
	sb_t *sb = (sb_t *)p;
	sb_ct1745_mixer_t *mixer = &sb->mixer_sb16;

	int c;

	opl3_update2(&sb->opl);
	emu8k_update(&sb->emu8k);
	sb_dsp_update(&sb->dsp);
	const int dsp_rec_pos = sb->dsp.record_pos_write;
	for (c = 0; c < len * 2; c += 2) {
		int32_t out_l, out_r, in_l, in_r;
		int c_emu8k = (((c / 2) * 44100) / 48000) * 2;

		out_l = ((((sb->opl.buffer[c] * mixer->fm_l) >> 15) * (sb->opl_emu ? 47000 : 51000)) >> 16);
		out_r = ((((sb->opl.buffer[c + 1] * mixer->fm_r) >> 15) * (sb->opl_emu ? 47000 : 51000)) >> 16);

		out_l += ((sb->emu8k.buffer[c_emu8k] * mixer->fm_l) >> 15);
		out_r += ((sb->emu8k.buffer[c_emu8k + 1] * mixer->fm_r) >> 15);

		/*TODO: multi-recording mic with agc/+20db, cd and line in with channel inversion  */
		in_l = (mixer->input_selector_left & INPUT_MIDI_L) ? out_l : 0 + (mixer->input_selector_left & INPUT_MIDI_R) ? out_r : 0;
		in_r = (mixer->input_selector_right & INPUT_MIDI_L) ? out_l : 0 + (mixer->input_selector_right & INPUT_MIDI_R) ? out_r : 0;

		out_l += ((int32_t)(low_fir_sb16(0, (float)sb->dsp.buffer[c]) * mixer->voice_l) / 3) >> 15;
		out_r += ((int32_t)(low_fir_sb16(1, (float)sb->dsp.buffer[c + 1]) * mixer->voice_r) / 3) >> 15;

		out_l = (out_l * mixer->master_l) >> 15;
		out_r = (out_r * mixer->master_r) >> 15;

		if (mixer->bass_l != 8 || mixer->bass_r != 8 || mixer->treble_l != 8 || mixer->treble_r != 8) {
			/* This is not exactly how one does bass/treble controls, but the end result is like it. A better implementation would reduce the cpu usage */
			if (mixer->bass_l > 8)
				out_l += (int32_t)(low_iir(0, (float)out_l) * sb_bass_treble_4bits[mixer->bass_l]);
			if (mixer->bass_r > 8)
				out_r += (int32_t)(low_iir(1, (float)out_r) * sb_bass_treble_4bits[mixer->bass_r]);
			if (mixer->treble_l > 8)
				out_l += (int32_t)(high_iir(0, (float)out_l) * sb_bass_treble_4bits[mixer->treble_l]);
			if (mixer->treble_r > 8)
				out_r += (int32_t)(high_iir(1, (float)out_r) * sb_bass_treble_4bits[mixer->treble_r]);
			if (mixer->bass_l < 8)
				out_l = (int32_t)(out_l * sb_bass_treble_4bits[mixer->bass_l] + low_cut_iir(0, (float)out_l) * (1.f - sb_bass_treble_4bits[mixer->bass_l]));
			if (mixer->bass_r < 8)
				out_r = (int32_t)(out_r * sb_bass_treble_4bits[mixer->bass_r] + low_cut_iir(1, (float)out_r) * (1.f - sb_bass_treble_4bits[mixer->bass_r]));
			if (mixer->treble_l < 8)
				out_l = (int32_t)(out_l * sb_bass_treble_4bits[mixer->treble_l] + high_cut_iir(0, (float)out_l) * (1.f - sb_bass_treble_4bits[mixer->treble_l]));
			if (mixer->treble_r < 8)
				out_r = (int32_t)(out_r * sb_bass_treble_4bits[mixer->treble_r] + high_cut_iir(1, (float)out_r) * (1.f - sb_bass_treble_4bits[mixer->treble_r]));
		}
		if (sb->dsp.sb_enable_i) {
//                      in_l += (mixer->input_selector_left&INPUT_CD_L) ? audio_cd_buffer[cd_read_pos+c_emu8k] : 0 + (mixer->input_selector_left&INPUT_CD_R) ? audio_cd_buffer[cd_read_pos+c_emu8k+1] : 0;
//                      in_r += (mixer->input_selector_right&INPUT_CD_L) ? audio_cd_buffer[cd_read_pos+c_emu8k]: 0 + (mixer->input_selector_right&INPUT_CD_R) ? audio_cd_buffer[cd_read_pos+c_emu8k+1] : 0;

			int c_record = dsp_rec_pos;
			c_record += (((c / 2) * sb->dsp.sb_freq) / 48000) * 2;
#ifdef SB_DSP_RECORD_DEBUG
			if (c_record > 0xFFFF && !buf_written)
			{
				if (!soundfsb) soundfsb=fopen("sound_sb.pcm","wb");
				fwrite(sb->dsp.record_buffer,2,0x10000,soundfsb);
				old_dsp_rec_pos = dsp_rec_pos;
				buf_written=1;
			}
#endif
			in_l <<= mixer->input_gain_L;
			in_r <<= mixer->input_gain_R;
			// Clip signal
			if (in_l < -32768)
				in_l = -32768;
			else if (in_l > 32767)
				in_l = 32767;

			if (in_r < -32768)
				in_r = -32768;
			else if (in_r > 32767)
				in_r = 32767;
			sb->dsp.record_buffer[c_record & 0xFFFF] = in_l;
			sb->dsp.record_buffer[(c_record + 1) & 0xFFFF] = in_r;
#ifdef SB_DSP_RECORD_DEBUG
			if (c_record != last_crecord)
			{
				if (!soundfsbin) soundfsbin=fopen("sound_sb_in.pcm","wb");
				fwrite(&sb->dsp.record_buffer[c_record&0xFFFF],2,2,soundfsbin);
				last_crecord=c_record;
			}
#endif
		}

		buffer[c] += (out_l << mixer->output_gain_L);
		buffer[c + 1] += (out_r << mixer->output_gain_R);
	}
#ifdef SB_DSP_RECORD_DEBUG
	if (old_dsp_rec_pos > dsp_rec_pos)
	{
		buf_written=0;
		old_dsp_rec_pos=dsp_rec_pos;
	}
#endif

	sb->dsp.record_pos_write += ((len * sb->dsp.sb_freq) / 48000) * 2;
	sb->dsp.record_pos_write &= 0xFFFF;
	sb->pos = 0;
	sb->opl.pos = 0;
	sb->dsp.pos = 0;
	sb->emu8k.pos = 0;
}

void sb_ct1335_mixer_write(uint16_t addr, uint8_t val, void *p) {
	sb_t *sb = (sb_t *)p;
	sb_ct1335_mixer_t *mixer = &sb->mixer_sb2;

	if (!(addr & 1)) {
		mixer->index = val;
		mixer->regs[0x01] = val;
	} else {
		if (mixer->index == 0) {
			/* Reset */
			mixer->regs[0x02] = 4 << 1;
			mixer->regs[0x06] = 4 << 1;
			mixer->regs[0x08] = 0 << 1;
			/* changed default from -46dB to 0dB*/
			mixer->regs[0x0A] = 3 << 1;
		} else {
			mixer->regs[mixer->index] = val;
			switch (mixer->index) {
			case 0x00:
			case 0x02:
			case 0x06:
			case 0x08:
			case 0x0A:break;

			default:pclog("sb_ct1335: Unknown register WRITE: %02X\t%02X\n", mixer->index, mixer->regs[mixer->index]);
				break;
			}
		}
		mixer->master = sb_att_4dbstep_3bits[(mixer->regs[0x02] >> 1) & 0x7];
		mixer->fm = sb_att_4dbstep_3bits[(mixer->regs[0x06] >> 1) & 0x7];
		mixer->cd = sb_att_4dbstep_3bits[(mixer->regs[0x08] >> 1) & 0x7];
		mixer->voice = sb_att_7dbstep_2bits[(mixer->regs[0x0A] >> 1) & 0x3];

		sound_set_cd_volume(((uint32_t)mixer->master * (uint32_t)mixer->cd) / 65535,
				    ((uint32_t)mixer->master * (uint32_t)mixer->cd) / 65535);
	}
}

uint8_t sb_ct1335_mixer_read(uint16_t addr, void *p) {
	sb_t *sb = (sb_t *)p;
	sb_ct1335_mixer_t *mixer = &sb->mixer_sb2;

	if (!(addr & 1))
		return mixer->index;

	switch (mixer->index) {
	case 0x00:
	case 0x02:
	case 0x06:
	case 0x08:
	case 0x0A:return mixer->regs[mixer->index];
	default:pclog("sb_ct1335: Unknown register READ: %02X\t%02X\n", mixer->index, mixer->regs[mixer->index]);
		break;
	}

	return 0xff;
}

void sb_ct1335_mixer_reset(sb_t *sb) {
	sb_ct1335_mixer_write(0x254, 0, sb);
	sb_ct1335_mixer_write(0x255, 0, sb);
}

void sb_ct1345_mixer_write(uint16_t addr, uint8_t val, void *p) {
	sb_t *sb = (sb_t *)p;
	sb_ct1345_mixer_t *mixer = &sb->mixer_sbpro;

	if (!(addr & 1)) {
		mixer->index = val;
		mixer->regs[0x01] = val;
	} else {
		if (mixer->index == 0) {
			/* Reset */
			mixer->regs[0x0A] = 0 << 1;
			mixer->regs[0x0C] = (0 << 5) | (0 << 3) | (0 << 1);
			mixer->regs[0x0E] = (0 << 5) | (0 << 1);
			/* changed default from -11dB to 0dB */
			mixer->regs[0x04] = (7 << 5) | (7 << 1);
			mixer->regs[0x22] = (7 << 5) | (7 << 1);
			mixer->regs[0x26] = (7 << 5) | (7 << 1);
			mixer->regs[0x28] = (0 << 5) | (0 << 1);
			mixer->regs[0x2E] = (0 << 5) | (0 << 1);
			sb_dsp_set_stereo(&sb->dsp, mixer->regs[0x0E] & 2);
		} else {
			mixer->regs[mixer->index] = val;
			switch (mixer->index) {
				/* Compatibility: chain registers 0x02 and 0x22 as well as 0x06 and 0x26 */
			case 0x02:
			case 0x06:mixer->regs[mixer->index + 0x20] = ((val & 0xE) << 4) | (val & 0xE);
				break;

			case 0x22:
			case 0x26:mixer->regs[mixer->index - 0x20] = (val & 0xE);
				break;

				/* More compatibility:  SoundBlaster Pro selects register 020h for 030h, 022h for 032h, 026h for 036h,028h for 038h. */
			case 0x30:
			case 0x32:
			case 0x36:
			case 0x38:mixer->regs[mixer->index - 0x10] = (val & 0xEE);
				break;

			case 0x00:
			case 0x04:
			case 0x0a:
			case 0x0c:
			case 0x0e:
			case 0x28:
			case 0x2e:break;

			default:pclog("sb_ct1345: Unknown register WRITE: %02X\t%02X\n", mixer->index, mixer->regs[mixer->index]);
				break;
			}
		}

		mixer->voice_l = sb_att_4dbstep_3bits[(mixer->regs[0x04] >> 5) & 0x7];
		mixer->voice_r = sb_att_4dbstep_3bits[(mixer->regs[0x04] >> 1) & 0x7];
		mixer->master_l = sb_att_4dbstep_3bits[(mixer->regs[0x22] >> 5) & 0x7];
		mixer->master_r = sb_att_4dbstep_3bits[(mixer->regs[0x22] >> 1) & 0x7];
		mixer->fm_l = sb_att_4dbstep_3bits[(mixer->regs[0x26] >> 5) & 0x7];
		mixer->fm_r = sb_att_4dbstep_3bits[(mixer->regs[0x26] >> 1) & 0x7];
		mixer->cd_l = sb_att_4dbstep_3bits[(mixer->regs[0x28] >> 5) & 0x7];
		mixer->cd_r = sb_att_4dbstep_3bits[(mixer->regs[0x28] >> 1) & 0x7];
		mixer->line_l = sb_att_4dbstep_3bits[(mixer->regs[0x2E] >> 5) & 0x7];
		mixer->line_r = sb_att_4dbstep_3bits[(mixer->regs[0x2E] >> 1) & 0x7];

		mixer->mic = sb_att_7dbstep_2bits[(mixer->regs[0x0A] >> 1) & 0x3];

		mixer->output_filter = !(mixer->regs[0xE] & 0x20);
		mixer->input_filter = !(mixer->regs[0xC] & 0x20);
		mixer->in_filter_freq = ((mixer->regs[0xC] & 0x8) == 0) ? 3200 : 8800;
		mixer->stereo = mixer->regs[0xE] & 2;
		if (mixer->index == 0xE)
			sb_dsp_set_stereo(&sb->dsp, val & 2);

		switch ((mixer->regs[0xc] & 6)) {
		case 2:mixer->input_selector = INPUT_CD_L | INPUT_CD_R;
			break;
		case 6:mixer->input_selector = INPUT_LINE_L | INPUT_LINE_R;
			break;
		default:mixer->input_selector = INPUT_MIC;
			break;
		}

		/* TODO: pcspeaker volume? Or is it not worth? */
		sound_set_cd_volume(((uint32_t)mixer->master_l * (uint32_t)mixer->cd_l) / 65535,
				    ((uint32_t)mixer->master_r * (uint32_t)mixer->cd_r) / 65535);
	}
}

uint8_t sb_ct1345_mixer_read(uint16_t addr, void *p) {
	sb_t *sb = (sb_t *)p;
	sb_ct1345_mixer_t *mixer = &sb->mixer_sbpro;

	if (!(addr & 1))
		return mixer->index;

	switch (mixer->index) {
	case 0x00:
	case 0x04:
	case 0x0a:
	case 0x0c:
	case 0x0e:
	case 0x22:
	case 0x26:
	case 0x28:
	case 0x2e:
	case 0x02:
	case 0x06:
	case 0x30:
	case 0x32:
	case 0x36:
	case 0x38:return mixer->regs[mixer->index];

	default:pclog("sb_ct1345: Unknown register READ: %02X\t%02X\n", mixer->index, mixer->regs[mixer->index]);
		break;
	}

	return 0xff;
}
void sb_ct1345_mixer_reset(sb_t *sb) {
	sb_ct1345_mixer_write(4, 0, sb);
	sb_ct1345_mixer_write(5, 0, sb);
}

void sb_ct1745_mixer_write(uint16_t addr, uint8_t val, void *p) {
	sb_t *sb = (sb_t *)p;
	sb_ct1745_mixer_t *mixer = &sb->mixer_sb16;

	if (!(addr & 1)) {
		mixer->index = val;
	} else {
		// TODO: and this?  001h:
		/*DESCRIPTION
	 Contains previously selected register value.  Mixer Data Register value
	     NOTES
	 * SoundBlaster 16 sets bit 7 if previous mixer index invalid.
	 * Status bytes initially 080h on startup for all but level bytes (SB16)
		 */

		if (mixer->index == 0) {
			/* Reset */
			/* Changed defaults from -14dB to 0dB*/
			mixer->regs[0x30] = 31 << 3;
			mixer->regs[0x31] = 31 << 3;
			mixer->regs[0x32] = 31 << 3;
			mixer->regs[0x33] = 31 << 3;
			mixer->regs[0x34] = 31 << 3;
			mixer->regs[0x35] = 31 << 3;
			mixer->regs[0x36] = 0 << 3;
			mixer->regs[0x37] = 0 << 3;
			mixer->regs[0x38] = 0 << 3;
			mixer->regs[0x39] = 0 << 3;

			mixer->regs[0x3A] = 0 << 3;
			mixer->regs[0x3B] = 0 << 6;
			mixer->regs[0x3C] = OUTPUT_MIC | OUTPUT_CD_R | OUTPUT_CD_L | OUTPUT_LINE_R | OUTPUT_LINE_L;
			mixer->regs[0x3D] = INPUT_MIC | INPUT_CD_L | INPUT_LINE_L | INPUT_MIDI_L;
			mixer->regs[0x3E] = INPUT_MIC | INPUT_CD_R | INPUT_LINE_R | INPUT_MIDI_R;

			mixer->regs[0x3F] = mixer->regs[0x40] = 0 << 6;
			mixer->regs[0x41] = mixer->regs[0x42] = 0 << 6;

			mixer->regs[0x44] = mixer->regs[0x45] = 8 << 4;
			mixer->regs[0x46] = mixer->regs[0x47] = 8 << 4;

			mixer->regs[0x43] = 0;
		} else {
			mixer->regs[mixer->index] = val;
		}
		switch (mixer->index) {
			/* SBPro compatibility. Copy values to sb16 registers. */
		case 0x22:mixer->regs[0x30] = (mixer->regs[0x22] & 0xF0) | 0x8;
			mixer->regs[0x31] = ((mixer->regs[0x22] & 0xf) << 4) | 0x8;
			break;
		case 0x04:mixer->regs[0x32] = (mixer->regs[0x04] & 0xF0) | 0x8;
			mixer->regs[0x33] = ((mixer->regs[0x04] & 0xf) << 4) | 0x8;
			break;
		case 0x26:mixer->regs[0x34] = (mixer->regs[0x26] & 0xF0) | 0x8;
			mixer->regs[0x35] = ((mixer->regs[0x26] & 0xf) << 4) | 0x8;
			break;
		case 0x28:mixer->regs[0x36] = (mixer->regs[0x28] & 0xF0) | 0x8;
			mixer->regs[0x37] = ((mixer->regs[0x28] & 0xf) << 4) | 0x8;
			break;
		case 0x2E:mixer->regs[0x38] = (mixer->regs[0x2E] & 0xF0) | 0x8;
			mixer->regs[0x39] = ((mixer->regs[0x2E] & 0xf) << 4) | 0x8;
			break;
		case 0x0A:mixer->regs[0x3A] = (mixer->regs[0x0A] * 3) + 10;
			break;

			/*
			 (DSP 4.xx feature) The Interrupt Setup register, addressed as register 80h on the Mixer register map, is used to configure or determine the Interrupt request line. The DMA setup register, addressed as register 81h on the Mixer register map, is used to configure or determine the DMA channels.

			 Note: Registers 80h and 81h are Read-only for PnP boards.
			 */
		case 0x80:
			if (val & 1)
				sb_dsp_setirq(&sb->dsp, 2);
			if (val & 2)
				sb_dsp_setirq(&sb->dsp, 5);
			if (val & 4)
				sb_dsp_setirq(&sb->dsp, 7);
			if (val & 8)
				sb_dsp_setirq(&sb->dsp, 10);
			break;

		case 0x81:
			/* The documentation is confusing. sounds as if multple dma8 channels could be set. */
			if (val & 1)
				sb_dsp_setdma8(&sb->dsp, 0);
			if (val & 2)
				sb_dsp_setdma8(&sb->dsp, 1);
			if (val & 8)
				sb_dsp_setdma8(&sb->dsp, 3);
			if (val & 0x20)
				sb_dsp_setdma16(&sb->dsp, 5);
			if (val & 0x40)
				sb_dsp_setdma16(&sb->dsp, 6);
			if (val & 0x80)
				sb_dsp_setdma16(&sb->dsp, 7);
			break;
		}

		mixer->output_selector = mixer->regs[0x3C];
		mixer->input_selector_left = mixer->regs[0x3D];
		mixer->input_selector_right = mixer->regs[0x3E];

		mixer->master_l = sb_att_2dbstep_5bits[mixer->regs[0x30] >> 3];
		mixer->master_r = sb_att_2dbstep_5bits[mixer->regs[0x31] >> 3];
		mixer->voice_l = sb_att_2dbstep_5bits[mixer->regs[0x32] >> 3];
		mixer->voice_r = sb_att_2dbstep_5bits[mixer->regs[0x33] >> 3];
		mixer->fm_l = sb_att_2dbstep_5bits[mixer->regs[0x34] >> 3];
		mixer->fm_r = sb_att_2dbstep_5bits[mixer->regs[0x35] >> 3];
		mixer->cd_l = (mixer->output_selector & OUTPUT_CD_L) ? sb_att_2dbstep_5bits[mixer->regs[0x36] >> 3] : 0;
		mixer->cd_r = (mixer->output_selector & OUTPUT_CD_R) ? sb_att_2dbstep_5bits[mixer->regs[0x37] >> 3] : 0;
		mixer->line_l = (mixer->output_selector & OUTPUT_LINE_L) ? sb_att_2dbstep_5bits[mixer->regs[0x38] >> 3] : 0;
		mixer->line_r = (mixer->output_selector & OUTPUT_LINE_R) ? sb_att_2dbstep_5bits[mixer->regs[0x39] >> 3] : 0;

		mixer->mic = sb_att_2dbstep_5bits[mixer->regs[0x3A] >> 3];
		mixer->speaker = sb_att_2dbstep_5bits[mixer->regs[0x3B] * 3 + 22];

		mixer->input_gain_L = (mixer->regs[0x3F] >> 6);
		mixer->input_gain_R = (mixer->regs[0x40] >> 6);
		mixer->output_gain_L = (mixer->regs[0x41] >> 6);
		mixer->output_gain_R = (mixer->regs[0x42] >> 6);

		mixer->bass_l = mixer->regs[0x46] >> 4;
		mixer->bass_r = mixer->regs[0x47] >> 4;
		mixer->treble_l = mixer->regs[0x44] >> 4;
		mixer->treble_r = mixer->regs[0x45] >> 4;

		/*TODO: pcspeaker volume, with "output_selector" check? or better not? */
		sound_set_cd_volume(((uint32_t)mixer->master_l * (uint32_t)mixer->cd_l) / 65535,
				    ((uint32_t)mixer->master_r * (uint32_t)mixer->cd_r) / 65535);
//                pclog("sb_ct1745: Received register WRITE: %02X\t%02X\n", mixer->index, mixer->regs[mixer->index]);
	}
}

uint8_t sb_ct1745_mixer_read(uint16_t addr, void *p) {
	sb_t *sb = (sb_t *)p;
	sb_ct1745_mixer_t *mixer = &sb->mixer_sb16;

	if (!(addr & 1))
		return mixer->index;

//        pclog("sb_ct1745: received register READ: %02X\t%02X\n", mixer->index, mixer->regs[mixer->index]);

	if (mixer->index >= 0x30 && mixer->index <= 0x47) {
		return mixer->regs[mixer->index];
	}
	switch (mixer->index) {
	case 0x00:return mixer->regs[mixer->index];

		/*SB Pro compatibility*/
	case 0x04:return ((mixer->regs[0x33] >> 4) & 0x0f) | (mixer->regs[0x32] & 0xf0);
	case 0x0a:return (mixer->regs[0x3a] - 10) / 3;
	case 0x22:return ((mixer->regs[0x31] >> 4) & 0x0f) | (mixer->regs[0x30] & 0xf0);
	case 0x26:return ((mixer->regs[0x35] >> 4) & 0x0f) | (mixer->regs[0x34] & 0xf0);
	case 0x28:return ((mixer->regs[0x37] >> 4) & 0x0f) | (mixer->regs[0x36] & 0xf0);
	case 0x2e:return ((mixer->regs[0x39] >> 4) & 0x0f) | (mixer->regs[0x38] & 0xf0);

	case 0x48:
		// Undocumented. The Creative Windows Mixer calls this after calling 3C (input selector). even when writing.
		// Also, the version I have (5.17) does not use the MIDI.L/R input selectors. it uses the volume to mute (Affecting the output, obviously)
		return mixer->regs[mixer->index];

	case 0x80:
		/*TODO: Unaffected by mixer reset or soft reboot.
		 * Enabling multiple bits enables multiple IRQs.
		 */

		switch (sb->dsp.sb_irqnum) {
		case 2:return 1;
		case 5:return 2;
		case 7:return 4;
		case 10:return 8;
		}
		break;

	case 0x81: {
		/* TODO: Unaffected by mixer reset or soft reboot.
		* Enabling multiple 8 or 16-bit DMA bits enables multiple DMA channels.
		* Disabling all 8-bit DMA channel bits disables 8-bit DMA requests,
		    including translated 16-bit DMA requests.
		* Disabling all 16-bit DMA channel bits enables translation of 16-bit DMA
		    requests to 8-bit ones, using the selected 8-bit DMA channel.*/

		uint8_t result = 0;
		switch (sb->dsp.sb_8_dmanum) {
		case 0:result |= 1;
			break;
		case 1:result |= 2;
			break;
		case 3:result |= 8;
			break;
		}
		switch (sb->dsp.sb_16_dmanum) {
		case 5:result |= 0x20;
			break;
		case 6:result |= 0x40;
			break;
		case 7:result |= 0x80;
			break;
		}
		return result;
	}

		/* The Interrupt status register, addressed as register 82h on the Mixer register map,
		 is used by the ISR to determine whether the interrupt is meant for it or for some other ISR,
		 in which case it should chain to the previous routine.
		 */
	case 0x82:
		/* 0 = none, 1 =  digital 8bit or SBMIDI, 2 = digital 16bit, 4 = MPU-401 */
		/* 0x02000 DSP v4.04, 0x4000 DSP v4.05 0x8000 DSP v4.12. I haven't seen this making any difference, but I'm keeping it for now. */
		return ((sb->dsp.sb_irq8) ? 1 : 0) | ((sb->dsp.sb_irq16) ? 2 : 0) | 0x4000;

		/* TODO: creative drivers read and write on 0xFE and 0xFF. not sure what they are supposed to be. */


	default:pclog("sb_ct1745: Unknown register READ: %02X\t%02X\n", mixer->index, mixer->regs[mixer->index]);
		break;
	}

	return 0xff;
}

void sb_ct1745_mixer_reset(sb_t *sb) {
	sb_ct1745_mixer_write(4, 0, sb);
	sb_ct1745_mixer_write(5, 0, sb);
}

static uint16_t sb_mcv_addr[8] = {0x200, 0x210, 0x220, 0x230, 0x240, 0x250, 0x260, 0x270};

uint8_t sb_mcv_read(int port, void *p) {
	sb_t *sb = (sb_t *)p;

	pclog("sb_mcv_read: port=%04x\n", port);

	return sb->pos_regs[port & 7];
}

void sb_mcv_write(int port, uint8_t val, void *p) {
	uint16_t addr;
	sb_t *sb = (sb_t *)p;

	if (port < 0x102)
		return;

	pclog("sb_mcv_write: port=%04x val=%02x\n", port, val);

	addr = sb_mcv_addr[sb->pos_regs[4] & 7];
	io_removehandler(addr + 8, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
	io_removehandler(0x0388, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
	/* DSP I/O handler is activated in sb_dsp_setaddr */
	sb_dsp_setaddr(&sb->dsp, 0);

	sb->pos_regs[port & 7] = val;

	if (sb->pos_regs[2] & 1) {
		addr = sb_mcv_addr[sb->pos_regs[4] & 7];

		io_sethandler(addr + 8, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
		io_sethandler(0x0388, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
		/* DSP I/O handler is activated in sb_dsp_setaddr */
		sb_dsp_setaddr(&sb->dsp, addr);
	}
}

static int sb_pro_mcv_irqs[4] = {7, 5, 3, 3};

uint8_t sb_pro_mcv_read(int port, void *p) {
	sb_t *sb = (sb_t *)p;

	pclog("sb_pro_mcv_read: port=%04x\n", port);

	return sb->pos_regs[port & 7];
}

void sb_pro_mcv_write(int port, uint8_t val, void *p) {
	uint16_t addr;
	sb_t *sb = (sb_t *)p;

	if (port < 0x102)
		return;

	pclog("sb_pro_mcv_write: port=%04x val=%02x\n", port, val);

	addr = (sb->pos_regs[2] & 0x20) ? 0x220 : 0x240;
	io_removehandler(addr + 0, 0x0004, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
	io_removehandler(addr + 8, 0x0002, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
	io_removehandler(0x0388, 0x0004, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
	io_removehandler(addr + 4, 0x0002, sb_ct1345_mixer_read, NULL, NULL, sb_ct1345_mixer_write, NULL, NULL, sb);
	/* DSP I/O handler is activated in sb_dsp_setaddr */
	sb_dsp_setaddr(&sb->dsp, 0);

	sb->pos_regs[port & 7] = val;

	if (sb->pos_regs[2] & 1) {
		addr = (sb->pos_regs[2] & 0x20) ? 0x220 : 0x240;

		io_sethandler(addr + 0, 0x0004, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
		io_sethandler(addr + 8, 0x0002, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
		io_sethandler(0x0388, 0x0004, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
		io_sethandler(addr + 4, 0x0002, sb_ct1345_mixer_read, NULL, NULL, sb_ct1345_mixer_write, NULL, NULL, sb);
		/* DSP I/O handler is activated in sb_dsp_setaddr */
		sb_dsp_setaddr(&sb->dsp, addr);
	}
	sb_dsp_setirq(&sb->dsp, sb_pro_mcv_irqs[(sb->pos_regs[5] >> 4) & 3]);
	sb_dsp_setdma8(&sb->dsp, sb->pos_regs[4] & 3);
}

void *sb_1_init() {
	/*sb1/2 port mappings, 210h to 260h in 10h steps
	  2x0 to 2x3 -> CMS chip
	  2x6, 2xA, 2xC, 2xE -> DSP chip
	  2x8, 2x9, 388 and 389 FM chip*/
	sb_t *sb = malloc(sizeof(sb_t));
	uint16_t addr = device_get_config_int("addr");
	memset(sb, 0, sizeof(sb_t));

	opl2_init(&sb->opl);
	sb_dsp_init(&sb->dsp, SB1, SB_SUBTYPE_DEFAULT, sb);
	sb_dsp_setaddr(&sb->dsp, addr);
	sb_dsp_setirq(&sb->dsp, device_get_config_int("irq"));
	sb_dsp_setdma8(&sb->dsp, device_get_config_int("dma"));
	/* CMS I/O handler is activated on the dedicated sound_cms module
	   DSP I/O handler is activated in sb_dsp_setaddr */
	io_sethandler(addr + 8, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
	io_sethandler(0x0388, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
	sound_add_handler(sb_get_buffer_sb2, sb);
	return sb;
}
void *sb_15_init() {
	/*sb1/2 port mappings, 210h to 260h in 10h steps
	  2x0 to 2x3 -> CMS chip
	  2x6, 2xA, 2xC, 2xE -> DSP chip
	  2x8, 2x9, 388 and 389 FM chip*/
	sb_t *sb = malloc(sizeof(sb_t));
	uint16_t addr = device_get_config_int("addr");
	memset(sb, 0, sizeof(sb_t));

	opl2_init(&sb->opl);
	sb_dsp_init(&sb->dsp, SB15, SB_SUBTYPE_DEFAULT, sb);
	sb_dsp_setaddr(&sb->dsp, addr);
	sb_dsp_setirq(&sb->dsp, device_get_config_int("irq"));
	sb_dsp_setdma8(&sb->dsp, device_get_config_int("dma"));
	/* CMS I/O handler is activated on the dedicated sound_cms module
	   DSP I/O handler is activated in sb_dsp_setaddr */
	io_sethandler(addr + 8, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
	io_sethandler(0x0388, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
	sound_add_handler(sb_get_buffer_sb2, sb);
	return sb;
}

void *sb_mcv_init() {
	/*sb1/2 port mappings, 210h to 260h in 10h steps
	  2x6, 2xA, 2xC, 2xE -> DSP chip
	  2x8, 2x9, 388 and 389 FM chip*/
	sb_t *sb = malloc(sizeof(sb_t));
	memset(sb, 0, sizeof(sb_t));

	opl2_init(&sb->opl);
	sb_dsp_init(&sb->dsp, SB15, SB_SUBTYPE_DEFAULT, sb);
	sb_dsp_setaddr(&sb->dsp, 0);//addr);
	sb_dsp_setirq(&sb->dsp, device_get_config_int("irq"));
	sb_dsp_setdma8(&sb->dsp, device_get_config_int("dma"));
	sound_add_handler(sb_get_buffer_sb2, sb);
	/* I/O handlers activated in sb_mcv_write */
	mca_add(sb_mcv_read, sb_mcv_write, NULL, sb);
	sb->pos_regs[0] = 0x84;
	sb->pos_regs[1] = 0x50;
	return sb;
}
void *sb_2_init() {
	/*sb2 port mappings. 220h or 240h.
	  2x0 to 2x3 -> CMS chip
	  2x6, 2xA, 2xC, 2xE -> DSP chip
	  2x8, 2x9, 388 and 389 FM chip
	"CD version" also uses 250h or 260h for
	  2x0 to 2x3 -> CDROM interface
	  2x4 to 2x5 -> Mixer interface*/
	/*My SB 2.0 mirrors the OPL2 at ports 2x0/2x1. Presumably this mirror is
	  disabled when the CMS chips are present.
	  This mirror may also exist on SB 1.5 & MCV, however I am unable to
	  test this. It shouldn't exist on SB 1.0 as the CMS chips are always
	  present there.
	  Syndicate requires this mirror for music to play.*/
	sb_t *sb = malloc(sizeof(sb_t));
	uint16_t addr = device_get_config_int("addr");
	memset(sb, 0, sizeof(sb_t));

	opl2_init(&sb->opl);
	sb_dsp_init(&sb->dsp, SB2, SB_SUBTYPE_DEFAULT, sb);
	sb_dsp_setaddr(&sb->dsp, addr);
	sb_dsp_setirq(&sb->dsp, device_get_config_int("irq"));
	sb_dsp_setdma8(&sb->dsp, device_get_config_int("dma"));
	sb_ct1335_mixer_reset(sb);
	/* CMS I/O handler is activated on the dedicated sound_cms module
	   DSP I/O handler is activated in sb_dsp_setaddr */
	if (!GAMEBLASTER)
		io_sethandler(addr, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
	io_sethandler(addr + 8, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
	io_sethandler(0x0388, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);

	int mixer_addr = device_get_config_int("mixaddr");
	if (mixer_addr > 0) {
		io_sethandler(mixer_addr + 4, 0x0002, sb_ct1335_mixer_read, NULL, NULL, sb_ct1335_mixer_write, NULL, NULL, sb);
		sound_add_handler(sb_get_buffer_sb2_mixer, sb);
	} else
		sound_add_handler(sb_get_buffer_sb2, sb);

	return sb;
}

void *sb_pro_v1_init() {
	/*sbpro port mappings. 220h or 240h.
	  2x0 to 2x3 -> FM chip, Left and Right (9*2 voices)
	  2x4 to 2x5 -> Mixer interface
	  2x6, 2xA, 2xC, 2xE -> DSP chip
	  2x8, 2x9, 388 and 389 FM chip (9 voices)
	  2x0+10 to 2x0+13 CDROM interface.*/
	sb_t *sb = malloc(sizeof(sb_t));
	uint16_t addr = device_get_config_int("addr");
	memset(sb, 0, sizeof(sb_t));

	opl2_init(&sb->opl);
	sb_dsp_init(&sb->dsp, SBPRO, SB_SUBTYPE_DEFAULT, sb);
	sb_dsp_setaddr(&sb->dsp, addr);
	sb_dsp_setirq(&sb->dsp, device_get_config_int("irq"));
	sb_dsp_setdma8(&sb->dsp, device_get_config_int("dma"));
	sb_ct1345_mixer_reset(sb);
	/* DSP I/O handler is activated in sb_dsp_setaddr */
	io_sethandler(addr + 0, 0x0002, opl2_l_read, NULL, NULL, opl2_l_write, NULL, NULL, &sb->opl);
	io_sethandler(addr + 2, 0x0002, opl2_r_read, NULL, NULL, opl2_r_write, NULL, NULL, &sb->opl);
	io_sethandler(addr + 8, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
	io_sethandler(0x0388, 0x0002, opl2_read, NULL, NULL, opl2_write, NULL, NULL, &sb->opl);
	io_sethandler(addr + 4, 0x0002, sb_ct1345_mixer_read, NULL, NULL, sb_ct1345_mixer_write, NULL, NULL, sb);
	sound_add_handler(sb_get_buffer_sbpro, sb);

	return sb;
}

void *sb_pro_v2_init() {
	/*sbpro port mappings. 220h or 240h.
	  2x0 to 2x3 -> FM chip (18 voices)
	  2x4 to 2x5 -> Mixer interface
	  2x6, 2xA, 2xC, 2xE -> DSP chip
	  2x8, 2x9, 388 and 389 FM chip (9 voices)
	  2x0+10 to 2x0+13 CDROM interface.*/
	sb_t *sb = malloc(sizeof(sb_t));
	memset(sb, 0, sizeof(sb_t));

	uint16_t addr = device_get_config_int("addr");
	sb->opl_emu = device_get_config_int("opl_emu");
	opl3_init(&sb->opl, sb->opl_emu);
	sb_dsp_init(&sb->dsp, SBPRO2, SB_SUBTYPE_DEFAULT, sb);
	sb_dsp_setaddr(&sb->dsp, addr);
	sb_dsp_setirq(&sb->dsp, device_get_config_int("irq"));
	sb_dsp_setdma8(&sb->dsp, device_get_config_int("dma"));
	sb_ct1345_mixer_reset(sb);
	/* DSP I/O handler is activated in sb_dsp_setaddr */
	io_sethandler(addr + 0, 0x0004, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
	io_sethandler(addr + 8, 0x0002, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
	io_sethandler(0x0388, 0x0004, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
	io_sethandler(addr + 4, 0x0002, sb_ct1345_mixer_read, NULL, NULL, sb_ct1345_mixer_write, NULL, NULL, sb);
	sound_add_handler(sb_get_buffer_sbpro, sb);

	return sb;
}

void *sb_pro_mcv_init() {
	/*sbpro port mappings. 220h or 240h.
	  2x0 to 2x3 -> FM chip, Left and Right (18 voices)
	  2x4 to 2x5 -> Mixer interface
	  2x6, 2xA, 2xC, 2xE -> DSP chip
	  2x8, 2x9, 388 and 389 FM chip (9 voices)*/
	sb_t *sb = malloc(sizeof(sb_t));
	memset(sb, 0, sizeof(sb_t));

	sb->opl_emu = device_get_config_int("opl_emu");
	opl3_init(&sb->opl, sb->opl_emu);
	sb_dsp_init(&sb->dsp, SBPRO2, SB_SUBTYPE_DEFAULT, sb);
	sb_ct1345_mixer_reset(sb);
	/* I/O handlers activated in sb_mcv_write */
	sound_add_handler(sb_get_buffer_sbpro, sb);

	/* I/O handlers activated in sb_pro_mcv_write */
	mca_add(sb_pro_mcv_read, sb_pro_mcv_write, NULL, sb);
	sb->pos_regs[0] = 0x03;
	sb->pos_regs[1] = 0x51;

	return sb;
}

void *sb_16_init() {
	sb_t *sb = malloc(sizeof(sb_t));
	memset(sb, 0, sizeof(sb_t));

	uint16_t addr = device_get_config_int("addr");
	sb->opl_emu = device_get_config_int("opl_emu");
	opl3_init(&sb->opl, sb->opl_emu);
	sb_dsp_init(&sb->dsp, SB16, SB_SUBTYPE_DEFAULT, sb);
	sb_dsp_setaddr(&sb->dsp, addr);
	// TODO: irq and dma options too?
	sb_ct1745_mixer_reset(sb);
	io_sethandler(addr, 0x0004, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
	io_sethandler(addr + 8, 0x0002, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
	io_sethandler(0x0388, 0x0004, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
	io_sethandler(addr + 4, 0x0002, sb_ct1745_mixer_read, NULL, NULL, sb_ct1745_mixer_write, NULL, NULL, sb);
	sound_add_handler(sb_get_buffer_sb16, sb);
	mpu401_uart_init(&sb->mpu, 0x330, -1, 0);

	return sb;
}

int sb_awe32_available() {
	return rom_present("awe32.raw");
}

void *sb_awe32_init() {
	sb_t *sb = malloc(sizeof(sb_t));
	int onboard_ram = device_get_config_int("onboard_ram");
	memset(sb, 0, sizeof(sb_t));

	uint16_t addr = device_get_config_int("addr");
	uint16_t emu_addr = device_get_config_int("emu_addr");
	sb->opl_emu = device_get_config_int("opl_emu");
	opl3_init(&sb->opl, sb->opl_emu);
	sb_dsp_init(&sb->dsp, SB16 + 1, SB_SUBTYPE_DEFAULT, sb);
	sb_dsp_setaddr(&sb->dsp, addr);
	// TODO: irq and dma options too?
	sb_ct1745_mixer_reset(sb);
	io_sethandler(addr, 0x0004, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
	io_sethandler(addr + 8, 0x0002, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
	io_sethandler(0x0388, 0x0004, opl3_read, NULL, NULL, opl3_write, NULL, NULL, &sb->opl);
	io_sethandler(addr + 4, 0x0002, sb_ct1745_mixer_read, NULL, NULL, sb_ct1745_mixer_write, NULL, NULL, sb);
	sound_add_handler(sb_get_buffer_emu8k, sb);
	mpu401_uart_init(&sb->mpu, 0x330, -1, 0);
	emu8k_init(&sb->emu8k, emu_addr, onboard_ram);

	return sb;
}

void sb_close(void *p) {
	sb_t *sb = (sb_t *)p;
	sb_dsp_close(&sb->dsp);
#ifdef SB_DSP_RECORD_DEBUG
	if (soundfsb != 0)
	{
	    fclose(soundfsb);
	    soundfsb=0;
	}
	if (soundfsbin!= 0)
	{
	    fclose(soundfsbin);
	    soundfsbin=0;
	}
#endif

	free(sb);
}

void sb_awe32_close(void *p) {
	sb_t *sb = (sb_t *)p;

	emu8k_close(&sb->emu8k);

	sb_close(sb);
}

void sb_speed_changed(void *p) {
	sb_t *sb = (sb_t *)p;

	sb_dsp_speed_changed(&sb->dsp);
}

void sb_add_status_info(char *s, int max_len, void *p) {
	sb_t *sb = (sb_t *)p;

	sb_dsp_add_status_info(s, max_len, &sb->dsp);
}

static device_config_t sb_config[] =
	{
		{
			.name = "addr",
			.description = "Address",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "0x210",
						.value = 0x210
					},
					{
						.description = "0x220",
						.value = 0x220
					},
					{
						.description = "0x230",
						.value = 0x230
					},
					{
						.description = "0x240",
						.value = 0x240
					},
					{
						.description = "0x250",
						.value = 0x250
					},
					{
						.description = "0x260",
						.value = 0x260
					},
					{
						.description = ""
					}
				},
			.default_int = 0x220
		},
		{
			.name = "irq",
			.description = "IRQ",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "IRQ 2",
						.value = 2
					},
					{
						.description = "IRQ 3",
						.value = 3
					},
					{
						.description = "IRQ 5",
						.value = 5
					},
					{
						.description = "IRQ 7",
						.value = 7
					},
					{
						.description = ""
					}
				},
			.default_int = 7
		},
		{
			.name = "dma",
			.description = "DMA",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "DMA 1",
						.value = 1
					},
					{
						.description = "DMA 3",
						.value = 3
					},
					{
						.description = ""
					}
				},
			.default_int = 1
		},
		{
			.type = -1
		}
	};

static device_config_t sb2_config[] =
	{
		{
			.name = "addr",
			.description = "Address",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "0x220",
						.value = 0x220
					},
					{
						.description = "0x240",
						.value = 0x240
					},
					{
						.description = ""
					}
				},
			.default_int = 0x220
		},
		{
			.name = "mixaddr",
			.description = "Mixer Address",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "No mixer",
						.value = 0
					},
					{
						.description = "0x250",
						.value = 0x250
					},
					{
						.description = "0x260",
						.value = 0x260
					},
					{
						.description = ""
					}
				},
			.default_int = 0
		},
		{
			.name = "irq",
			.description = "IRQ",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "IRQ 2",
						.value = 2
					},
					{
						.description = "IRQ 3",
						.value = 3
					},
					{
						.description = "IRQ 5",
						.value = 5
					},
					{
						.description = "IRQ 7",
						.value = 7
					},
					{
						.description = ""
					}
				},
			.default_int = 7
		},
		{
			.name = "dma",
			.description = "DMA",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "DMA 1",
						.value = 1
					},
					{
						.description = "DMA 3",
						.value = 3
					},
					{
						.description = ""
					}
				},
			.default_int = 1
		},
		{
			.type = -1
		}
	};

static device_config_t sb_mcv_config[] =
	{
		{
			.name = "irq",
			.description = "IRQ",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "IRQ 3",
						.value = 3
					},
					{
						.description = "IRQ 5",
						.value = 5
					},
					{
						.description = "IRQ 7",
						.value = 7
					},
					{
						.description = ""
					}
				},
			.default_int = 7
		},
		{
			.name = "dma",
			.description = "DMA",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "DMA 1",
						.value = 1
					},
					{
						.description = "DMA 3",
						.value = 3
					},
					{
						.description = ""
					}
				},
			.default_int = 1
		},
		{
			.type = -1
		}
	};

static device_config_t sb_pro_v1_config[] =
	{
		{
			.name = "addr",
			.description = "Address",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "0x220",
						.value = 0x220
					},
					{
						.description = "0x240",
						.value = 0x240
					},
					{
						.description = ""
					}
				},
			.default_int = 0x220
		},
		{
			.name = "irq",
			.description = "IRQ",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "IRQ 2",
						.value = 2
					},
					{
						.description = "IRQ 5",
						.value = 5
					},
					{
						.description = "IRQ 7",
						.value = 7
					},
					{
						.description = "IRQ 10",
						.value = 10
					},
					{
						.description = ""
					}
				},
			.default_int = 7
		},
		{
			.name = "dma",
			.description = "DMA",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "DMA 1",
						.value = 1
					},
					{
						.description = "DMA 3",
						.value = 3
					},
					{
						.description = ""
					}
				},
			.default_int = 1
		},
		{
			.type = -1
		}
	};

static device_config_t sb_pro_v2_config[] =
	{
		{
			.name = "addr",
			.description = "Address",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "0x220",
						.value = 0x220
					},
					{
						.description = "0x240",
						.value = 0x240
					},
					{
						.description = ""
					}
				},
			.default_int = 0x220
		},
		{
			.name = "irq",
			.description = "IRQ",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "IRQ 2",
						.value = 2
					},
					{
						.description = "IRQ 5",
						.value = 5
					},
					{
						.description = "IRQ 7",
						.value = 7
					},
					{
						.description = "IRQ 10",
						.value = 10
					},
					{
						.description = ""
					}
				},
			.default_int = 7
		},
		{
			.name = "dma",
			.description = "DMA",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "DMA 1",
						.value = 1
					},
					{
						.description = "DMA 3",
						.value = 3
					},
					{
						.description = ""
					}
				},
			.default_int = 1
		},
		{
			.name = "opl_emu",
			.description = "OPL emulator",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "DBOPL",
						.value = OPL_DBOPL
					},
					{
						.description = "NukedOPL",
						.value = OPL_NUKED
					},
				},
			.default_int = OPL_DBOPL
		},
		{
			.type = -1
		}
	};

static device_config_t sb_pro_mcv_config[] =
	{
		{
			.name = "opl_emu",
			.description = "OPL emulator",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "DBOPL",
						.value = OPL_DBOPL
					},
					{
						.description = "NukedOPL",
						.value = OPL_NUKED
					},
				},
			.default_int = OPL_DBOPL
		},
		{
			.type = -1
		}
	};

static device_config_t sb_16_config[] =
	{
		{
			.name = "addr",
			.description = "Address",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "0x220",
						.value = 0x220
					},
					{
						.description = "0x240",
						.value = 0x240
					},
					{
						.description = "0x260",
						.value = 0x260
					},
					{
						.description = "0x280",
						.value = 0x280
					},
					{
						.description = ""
					}
				},
			.default_int = 0x220
		},
		{
			.name = "midi",
			.description = "MIDI out device",
			.type = CONFIG_MIDI,
			.default_int = 0
		},
		{
			.name = "opl_emu",
			.description = "OPL emulator",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "DBOPL",
						.value = OPL_DBOPL
					},
					{
						.description = "NukedOPL",
						.value = OPL_NUKED
					},
				},
			.default_int = OPL_DBOPL
		},
		{
			.type = -1
		}
	};

static device_config_t sb_awe32_config[] =
	{
		{
			.name = "addr",
			.description = "Address",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "0x220",
						.value = 0x220
					},
					{
						.description = "0x240",
						.value = 0x240
					},
					{
						.description = "0x260",
						.value = 0x260
					},
					{
						.description = "0x280",
						.value = 0x280
					},
					{
						.description = ""
					}
				},
			.default_int = 0x220
		},
		{
			.name = "emu_addr",
			.description = "EMU8000 Address",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "0x620",
						.value = 0x620
					},
					{
						.description = "0x640",
						.value = 0x640
					},
					{
						.description = "0x660",
						.value = 0x660
					},
					{
						.description = "0x680",
						.value = 0x680
					},
					{
						.description = ""
					}
				},
			.default_int = 0x620
		},
		{.name = "midi",
			.description = "MIDI out device",
			.type = CONFIG_MIDI,
			.default_int = 0
		},
		{
			.name = "onboard_ram",
			.description = "Onboard RAM",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "None",
						.value = 0
					},
					{
						.description = "512 KB",
						.value = 512
					},
					{
						.description = "2 MB",
						.value = 2048
					},
					{
						.description = "8 MB",
						.value = 8192
					},
					{
						.description = "28 MB",
						.value = 28 * 1024
					},
					{
						.description = ""
					}
				},
			.default_int = 512
		},
		{
			.name = "opl_emu",
			.description = "OPL emulator",
			.type = CONFIG_SELECTION,
			.selection =
				{
					{
						.description = "DBOPL",
						.value = OPL_DBOPL
					},
					{
						.description = "NukedOPL",
						.value = OPL_NUKED
					},
				},
			.default_int = OPL_DBOPL
		},
		{
			.type = -1
		}
	};

device_t sb_1_device =
	{
		"Sound Blaster v1.0",
		0,
		sb_1_init,
		sb_close,
		NULL,
		sb_speed_changed,
		NULL,
		sb_add_status_info,
		sb_config
	};
device_t sb_15_device =
	{
		"Sound Blaster v1.5",
		0,
		sb_15_init,
		sb_close,
		NULL,
		sb_speed_changed,
		NULL,
		sb_add_status_info,
		sb_config
	};
device_t sb_mcv_device =
	{
		"Sound Blaster MCV",
		DEVICE_MCA,
		sb_mcv_init,
		sb_close,
		NULL,
		sb_speed_changed,
		NULL,
		sb_add_status_info,
		sb_mcv_config
	};
device_t sb_2_device =
	{
		"Sound Blaster v2.0",
		0,
		sb_2_init,
		sb_close,
		NULL,
		sb_speed_changed,
		NULL,
		sb_add_status_info,
		sb2_config
	};
device_t sb_pro_v1_device =
	{
		"Sound Blaster Pro v1",
		0,
		sb_pro_v1_init,
		sb_close,
		NULL,
		sb_speed_changed,
		NULL,
		sb_add_status_info,
		sb_pro_v1_config
	};
device_t sb_pro_v2_device =
	{
		"Sound Blaster Pro v2",
		0,
		sb_pro_v2_init,
		sb_close,
		NULL,
		sb_speed_changed,
		NULL,
		sb_add_status_info,
		sb_pro_v2_config
	};
device_t sb_pro_mcv_device =
	{
		"Sound Blaster Pro MCV",
		DEVICE_MCA,
		sb_pro_mcv_init,
		sb_close,
		NULL,
		sb_speed_changed,
		NULL,
		sb_add_status_info,
		sb_pro_mcv_config
	};
device_t sb_16_device =
	{
		"Sound Blaster 16",
		0,
		sb_16_init,
		sb_close,
		NULL,
		sb_speed_changed,
		NULL,
		sb_add_status_info,
		sb_16_config
	};
device_t sb_awe32_device =
	{
		"Sound Blaster AWE32",
		0,
		sb_awe32_init,
		sb_awe32_close,
		sb_awe32_available,
		sb_speed_changed,
		NULL,
		sb_add_status_info,
		sb_awe32_config
	};
