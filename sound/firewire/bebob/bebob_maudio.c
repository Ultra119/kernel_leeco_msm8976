/*
 * bebob_maudio.c - a part of driver for BeBoB based devices
 *
 * Copyright (c) 2013-2014 Takashi Sakamoto
 *
 * Licensed under the terms of the GNU General Public License, version 2.
 */

#include "./bebob.h"
#include <sound/control.h>

/*
 * Just powering on, Firewire 410/Audiophile/1814 and ProjectMix I/O wait to
 * download firmware blob. To enable these devices, drivers should upload
 * firmware blob and send a command to initialize configuration to factory
 * settings when completing uploading. Then these devices generate bus reset
 * and are recognized as new devices with the firmware.
 *
 * For streaming, both of output and input streams are needed for Firewire 410
 * and Ozonic. The single stream is OK for the other devices even if the clock
 * source is not SYT-Match (I note no devices use SYT-Match).
 *
 * Without streaming, the devices except for Firewire Audiophile can mix any
 * input and output. For this reason, Audiophile cannot be used as standalone
 * mixer.
 *
 * Firewire 1814 and ProjectMix I/O uses special firmware. It will be freezed
 * when receiving any commands which the firmware can't understand. These
 * devices utilize completely different system to control. It is some
 * write-transaction directly into a certain address. All of addresses for mixer
 * functionality is between 0xffc700700000 to 0xffc70070009c.
 */

#define MAUDIO_SPECIFIC_ADDRESS	0xffc700000000

#define METER_OFFSET		0x00600000

/* some device has sync info after metering data */
#define METER_SIZE_SPECIAL	84	/* with sync info */
#define METER_SIZE_FW410	76	/* with sync info */
#define METER_SIZE_AUDIOPHILE	60	/* with sync info */
#define METER_SIZE_SOLO		52	/* with sync info */
#define METER_SIZE_OZONIC	48
#define METER_SIZE_NRV10	80

/* labels for metering */
#define ANA_IN		"Analog In"
#define ANA_OUT		"Analog Out"
#define DIG_IN		"Digital In"
#define SPDIF_IN	"S/PDIF In"
#define ADAT_IN		"ADAT In"
#define DIG_OUT		"Digital Out"
#define SPDIF_OUT	"S/PDIF Out"
#define ADAT_OUT	"ADAT Out"
#define STRM_IN		"Stream In"
#define AUX_OUT		"Aux Out"
#define HP_OUT		"HP Out"
/* for NRV */
#define UNKNOWN_METER	"Unknown"

struct special_params {
	bool is1814;
	unsigned int clk_src;
	unsigned int dig_in_fmt;
	unsigned int dig_out_fmt;
	unsigned int clk_lock;
	struct snd_ctl_elem_id *ctl_id_sync;
};

static inline int
get_meter(struct snd_bebob *bebob, void *buf, unsigned int size)
{
	return snd_fw_transaction(bebob->unit, TCODE_READ_BLOCK_REQUEST,
				  MAUDIO_SPECIFIC_ADDRESS + METER_OFFSET,
				  buf, size, 0);
}

static int
check_clk_sync(struct snd_bebob *bebob, unsigned int size, bool *sync)
{
	int err;
	u8 *buf;

	buf = kmalloc(size, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	err = get_meter(bebob, buf, size);
	if (err < 0)
		goto end;

	/* if synced, this value is the same as SFC of FDF in CIP header */
	*sync = (buf[size - 2] != 0xff);
end:
	kfree(buf);
	return err;
}

/*
 * dig_fmt: 0x00:S/PDIF, 0x01:ADAT
 * clk_lock: 0x00:unlock, 0x01:lock
 */
static int
avc_maudio_set_special_clk(struct snd_bebob *bebob, unsigned int clk_src,
			   unsigned int dig_in_fmt, unsigned int dig_out_fmt,
			   unsigned int clk_lock)
{
	struct special_params *params = bebob->maudio_special_quirk;
	int err;
	u8 *buf;

	if (amdtp_stream_running(&bebob->rx_stream) ||
	    amdtp_stream_running(&bebob->tx_stream))
		return -EBUSY;

	buf = kmalloc(12, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	buf[0]  = 0x00;		/* CONTROL */
	buf[1]  = 0xff;		/* UNIT */
	buf[2]  = 0x00;		/* vendor dependent */
	buf[3]  = 0x04;		/* company ID high */
	buf[4]  = 0x00;		/* company ID middle */
	buf[5]  = 0x04;		/* company ID low */
	buf[6]  = 0xff & clk_src;	/* clock source */
	buf[7]  = 0xff & dig_in_fmt;	/* input digital format */
	buf[8]  = 0xff & dig_out_fmt;	/* output digital format */
	buf[9]  = 0xff & clk_lock;	/* lock these settings */
	buf[10] = 0x00;		/* padding  */
	buf[11] = 0x00;		/* padding */

	err = fcp_avc_transaction(bebob->unit, buf, 12, buf, 12,
				  BIT(1) | BIT(2) | BIT(3) | BIT(4) |
				  BIT(5) | BIT(6) | BIT(7) | BIT(8) |
				  BIT(9));
	if ((err > 0) && (err < 10))
		err = -EIO;
	else if (buf[0] == 0x08) /* NOT IMPLEMENTED */
		err = -ENOSYS;
	else if (buf[0] == 0x0a) /* REJECTED */
		err = -EINVAL;
	if (err < 0)
		goto end;

	params->clk_src		= buf[6];
	params->dig_in_fmt	= buf[7];
	params->dig_out_fmt	= buf[8];
	params->clk_lock	= buf[9];

	if (params->ctl_id_sync)
		snd_ctl_notify(bebob->card, SNDRV_CTL_EVENT_MASK_VALUE,
			       params->ctl_id_sync);

	err = 0;
end:
	kfree(buf);
	return err;
}
static void
special_stream_formation_set(struct snd_bebob *bebob)
{
	static const unsigned int ch_table[2][2][3] = {
		/* AMDTP_OUT_STREAM */
		{ {  6,  6,  4 },	/* SPDIF */
		  { 12,  8,  4 } },	/* ADAT */
		/* AMDTP_IN_STREAM */
		{ { 10, 10,  2 },	/* SPDIF */
		  { 16, 12,  2 } }	/* ADAT */
	};
	struct special_params *params = bebob->maudio_special_quirk;
	unsigned int i, max;

	max = SND_BEBOB_STRM_FMT_ENTRIES - 1;
	if (!params->is1814)
		max -= 2;

	for (i = 0; i < max; i++) {
		bebob->tx_stream_formations[i + 1].pcm =
			ch_table[AMDTP_IN_STREAM][params->dig_in_fmt][i / 2];
		bebob->tx_stream_formations[i + 1].midi = 1;

		bebob->rx_stream_formations[i + 1].pcm =
			ch_table[AMDTP_OUT_STREAM][params->dig_out_fmt][i / 2];
		bebob->rx_stream_formations[i + 1].midi = 1;
	}
}

static int add_special_controls(struct snd_bebob *bebob);
int
snd_bebob_maudio_special_discover(struct snd_bebob *bebob, bool is1814)
{
	struct special_params *params;
	int err;

	params = kzalloc(sizeof(struct special_params), GFP_KERNEL);
	if (params == NULL)
		return -ENOMEM;

	mutex_lock(&bebob->mutex);

	bebob->maudio_special_quirk = (void *)params;
	params->is1814 = is1814;

	/* initialize these parameters because driver is not allowed to ask */
	bebob->rx_stream.context = ERR_PTR(-1);
	bebob->tx_stream.context = ERR_PTR(-1);
	err = avc_maudio_set_special_clk(bebob, 0x03, 0x00, 0x00, 0x00);
	if (err < 0) {
		dev_err(&bebob->unit->device,
			"fail to initialize clock params: %d\n", err);
		goto end;
	}

	err = add_special_controls(bebob);
	if (err < 0)
		goto end;

	special_stream_formation_set(bebob);

	if (params->is1814) {
		bebob->midi_input_ports = 1;
		bebob->midi_output_ports = 1;
	} else {
		bebob->midi_input_ports = 2;
		bebob->midi_output_ports = 2;
	}
end:
	if (err < 0) {
		kfree(params);
		bebob->maudio_special_quirk = NULL;
	}
	mutex_unlock(&bebob->mutex);
	return err;
}

/* Input plug shows actual rate. Output plug is needless for this purpose. */
static int special_get_rate(struct snd_bebob *bebob, unsigned int *rate)
{
	int err, trials;

	trials = 0;
	do {
		err = avc_general_get_sig_fmt(bebob->unit, rate,
					      AVC_GENERAL_PLUG_DIR_IN, 0);
	} while (err == -EAGAIN && ++trials < 3);

	return err;
}
static int special_set_rate(struct snd_bebob *bebob, unsigned int rate)
{
	struct special_params *params = bebob->maudio_special_quirk;
	int err;

	err = avc_general_set_sig_fmt(bebob->unit, rate,
				      AVC_GENERAL_PLUG_DIR_OUT, 0);
	if (err < 0)
		goto end;

	/*
	 * Just after changing sampling rate for output, a followed command
	 * for input is easy to fail. This is a workaround fot this issue.
	 */
	msleep(100);

	err = avc_general_set_sig_fmt(bebob->unit, rate,
				      AVC_GENERAL_PLUG_DIR_IN, 0);
	if (err < 0)
		goto end;

	if (params->ctl_id_sync)
		snd_ctl_notify(bebob->card, SNDRV_CTL_EVENT_MASK_VALUE,
			       params->ctl_id_sync);
end:
	return err;
}

/* Clock source control for special firmware */
static char *const special_clk_labels[] = {
	SND_BEBOB_CLOCK_INTERNAL " with Digital Mute", "Digital",
	"Word Clock", SND_BEBOB_CLOCK_INTERNAL};
static int special_clk_get(struct snd_bebob *bebob, unsigned int *id)
{
	struct special_params *params = bebob->maudio_special_quirk;
	*id = params->clk_src;
	return 0;
}
static int special_clk_ctl_info(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_info *einf)
{
	einf->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	einf->count = 1;
	einf->value.enumerated.items = ARRAY_SIZE(special_clk_labels);

	if (einf->value.enumerated.item >= einf->value.enumerated.items)
		einf->value.enumerated.item = einf->value.enumerated.items - 1;

	strcpy(einf->value.enumerated.name,
	       special_clk_labels[einf->value.enumerated.item]);

	return 0;
}
static int special_clk_ctl_get(struct snd_kcontrol *kctl,
			       struct snd_ctl_elem_value *uval)
{
	struct snd_bebob *bebob = snd_kcontrol_chip(kctl);
	struct special_params *params = bebob->maudio_special_quirk;
	uval->value.enumerated.item[0] = params->clk_src;
	return 0;
}
static int special_clk_ctl_put(struct snd_kcontrol *kctl,
			       struct snd_ctl_elem_value *uval)
{
	struct snd_bebob *bebob = snd_kcontrol_chip(kctl);
	struct special_params *params = bebob->maudio_special_quirk;
	int err, id;

	mutex_lock(&bebob->mutex);

	id = uval->value.enumerated.item[0];
	if (id >= ARRAY_SIZE(special_clk_labels))
		return 0;

	err = avc_maudio_set_special_clk(bebob, id,
					 params->dig_in_fmt,
					 params->dig_out_fmt,
					 params->clk_lock);
	mutex_unlock(&bebob->mutex);

	return err >= 0;
}
static struct snd_kcontrol_new special_clk_ctl = {
	.name	= "Clock Source",
	.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
	.access	= SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info	= special_clk_ctl_info,
	.get	= special_clk_ctl_get,
	.put	= special_clk_ctl_put
};

/* Clock synchronization control for special firmware */
static int special_sync_ctl_info(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_info *einf)
{
	einf->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	einf->count = 1;
	einf->value.integer.min = 0;
	einf->value.integer.max = 1;

	return 0;
}
static int special_sync_ctl_get(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_value *uval)
{
	struct snd_bebob *bebob = snd_kcontrol_chip(kctl);
	int err;
	bool synced = 0;

	err = check_clk_sync(bebob, METER_SIZE_SPECIAL, &synced);
	if (err >= 0)
		uval->value.integer.value[0] = synced;

	return 0;
}
static struct snd_kcontrol_new special_sync_ctl = {
	.name	= "Sync Status",
	.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
	.access	= SNDRV_CTL_ELEM_ACCESS_READ,
	.info	= special_sync_ctl_info,
	.get	= special_sync_ctl_get,
};

/* Digital interface control for special firmware */
static char *const special_dig_iface_labels[] = {
	"S/PDIF Optical", "S/PDIF Coaxial", "ADAT Optical"
};
static int special_dig_in_iface_ctl_info(struct snd_kcontrol *kctl,
					 struct snd_ctl_elem_info *einf)
{
	einf->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	einf->count = 1;
	einf->value.enumerated.items = ARRAY_SIZE(special_dig_iface_labels);

	if (einf->value.enumerated.item >= einf->value.enumerated.items)
		einf->value.enumerated.item = einf->value.enumerated.items - 1;

	strcpy(einf->value.enumerated.name,
	       special_dig_iface_labels[einf->value.enumerated.item]);

	return 0;
}
static int special_dig_in_iface_ctl_get(struct snd_kcontrol *kctl,
					struct snd_ctl_elem_value *uval)
{
	struct snd_bebob *bebob = snd_kcontrol_chip(kctl);
	struct special_params *params = bebob->maudio_special_quirk;
	unsigned int dig_in_iface;
	int err, val;

	mutex_lock(&bebob->mutex);

	err = avc_audio_get_selector(bebob->unit, 0x00, 0x04,
				     &dig_in_iface);
	if (err < 0) {
		dev_err(&bebob->unit->device,
			"fail to get digital input interface: %d\n", err);
		goto end;
	}

	/* encoded id for user value */
	val = (params->dig_in_fmt << 1) | (dig_in_iface & 0x01);

	/* for ADAT Optical */
	if (val > 2)
		val = 2;

	uval->value.enumerated.item[0] = val;
end:
	mutex_unlock(&bebob->mutex);
	return err;
}
static int special_dig_in_iface_ctl_set(struct snd_kcontrol *kctl,
					struct snd_ctl_elem_value *uval)
{
	struct snd_bebob *bebob = snd_kcontrol_chip(kctl);
	struct special_params *params = bebob->maudio_special_quirk;
	unsigned int id, dig_in_fmt, dig_in_iface;
	int err;

	mutex_lock(&bebob->mutex);

	id = uval->value.enumerated.item[0];

	/* decode user value */
	dig_in_fmt = (id >> 1) & 0x01;
	dig_in_iface = id & 0x01;

	err = avc_maudio_set_special_clk(bebob,
					 params->clk_src,
					 dig_in_fmt,
					 params->dig_out_fmt,
					 params->clk_lock);
	if ((err < 0) || (params->dig_in_fmt > 0)) /* ADAT */
		goto end;

	err = avc_audio_set_selector(bebob->unit, 0x00, 0x04, dig_in_iface);
	if (err < 0)
		dev_err(&bebob->unit->device,
			"fail to set digital input interface: %d\n", err);
end:
	special_stream_formation_set(bebob);
	mutex_unlock(&bebob->mutex);
	return err;
}
static struct snd_kcontrol_new special_dig_in_iface_ctl = {
	.name	= "Digital Input Interface",
	.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
	.access	= SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info	= special_dig_in_iface_ctl_info,
	.get	= special_dig_in_iface_ctl_get,
	.put	= special_dig_in_iface_ctl_set
};

static int special_dig_out_iface_ctl_info(struct snd_kcontrol *kctl,
					  struct snd_ctl_elem_info *einf)
{
	einf->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	einf->count = 1;
	einf->value.enumerated.items = ARRAY_SIZE(special_dig_iface_labels) - 1;

	if (einf->value.enumerated.item >= einf->value.enumerated.items)
		einf->value.enumerated.item = einf->value.enumerated.items - 1;

	strcpy(einf->value.enumerated.name,
	       special_dig_iface_labels[einf->value.enumerated.item + 1]);

	return 0;
}
static int special_dig_out_iface_ctl_get(struct snd_kcontrol *kctl,
					 struct snd_ctl_elem_value *uval)
{
	struct snd_bebob *bebob = snd_kcontrol_chip(kctl);
	struct special_params *params = bebob->maudio_special_quirk;
	mutex_lock(&bebob->mutex);
	uval->value.enumerated.item[0] = params->dig_out_fmt;
	mutex_unlock(&bebob->mutex);
	return 0;
}
static int special_dig_out_iface_ctl_set(struct snd_kcontrol *kctl,
					 struct snd_ctl_elem_value *uval)
{
	struct snd_bebob *bebob = snd_kcontrol_chip(kctl);
	struct special_params *params = bebob->maudio_special_quirk;
	unsigned int id;
	int err;

	mutex_lock(&bebob->mutex);

	id = uval->value.enumerated.item[0];

	err = avc_maudio_set_special_clk(bebob,
					 params->clk_src,
					 params->dig_in_fmt,
					 id, params->clk_lock);
	if (err >= 0)
		special_stream_formation_set(bebob);

	mutex_unlock(&bebob->mutex);
	return err;
}
static struct snd_kcontrol_new special_dig_out_iface_ctl = {
	.name	= "Digital Output Interface",
	.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
	.access	= SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info	= special_dig_out_iface_ctl_info,
	.get	= special_dig_out_iface_ctl_get,
	.put	= special_dig_out_iface_ctl_set
};

static int add_special_controls(struct snd_bebob *bebob)
{
	struct snd_kcontrol *kctl;
	struct special_params *params = bebob->maudio_special_quirk;
	int err;

	kctl = snd_ctl_new1(&special_clk_ctl, bebob);
	err = snd_ctl_add(bebob->card, kctl);
	if (err < 0)
		goto end;

	kctl = snd_ctl_new1(&special_sync_ctl, bebob);
	err = snd_ctl_add(bebob->card, kctl);
	if (err < 0)
		goto end;
	params->ctl_id_sync = &kctl->id;

	kctl = snd_ctl_new1(&special_dig_in_iface_ctl, bebob);
	err = snd_ctl_add(bebob->card, kctl);
	if (err < 0)
		goto end;

	kctl = snd_ctl_new1(&special_dig_out_iface_ctl, bebob);
	err = snd_ctl_add(bebob->card, kctl);
end:
	return err;
}

/* Hardware metering for special firmware */
static char *const special_meter_labels[] = {
	ANA_IN, ANA_IN, ANA_IN, ANA_IN,
	SPDIF_IN,
	ADAT_IN, ADAT_IN, ADAT_IN, ADAT_IN,
	ANA_OUT, ANA_OUT,
	SPDIF_OUT,
	ADAT_OUT, ADAT_OUT, ADAT_OUT, ADAT_OUT,
	HP_OUT, HP_OUT,
	AUX_OUT
};
static int
special_meter_get(struct snd_bebob *bebob, u32 *target, unsigned int size)
{
	u16 *buf;
	unsigned int i, c, channels;
	int err;

	channels = ARRAY_SIZE(special_meter_labels) * 2;
	if (size < channels * sizeof(u32))
		return -EINVAL;

	/* omit last 4 bytes because it's clock info. */
	buf = kmalloc(METER_SIZE_SPECIAL - 4, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	err = get_meter(bebob, (void *)buf, METER_SIZE_SPECIAL - 4);
	if (err < 0)
		goto end;

	/* Its format is u16 and some channels are unknown. */
	i = 0;
	for (c = 2; c < channels + 2; c++)
		target[i++] = be16_to_cpu(buf[c]) << 16;
end:
	kfree(buf);
	return err;
}

/* last 4 bytes are omitted because it's clock info. */
static char *const fw410_meter_labels[] = {
	ANA_IN, DIG_IN,
	ANA_OUT, ANA_OUT, ANA_OUT, ANA_OUT, DIG_OUT,
	HP_OUT
};
static char *const audiophile_meter_labels[] = {
	ANA_IN, DIG_IN,
	ANA_OUT, ANA_OUT, DIG_OUT,
	HP_OUT, AUX_OUT,
};
static char *const solo_meter_labels[] = {
	ANA_IN, DIG_IN,
	STRM_IN, STRM_IN,
	ANA_OUT, DIG_OUT
};

/* no clock info */
static char *const ozonic_meter_labels[] = {
	ANA_IN, ANA_IN,
	STRM_IN, STRM_IN,
	ANA_OUT, ANA_OUT
};
/* TODO: need testers. these positions are based on authour's assumption */
static char *const nrv10_meter_labels[] = {
	ANA_IN, ANA_IN, ANA_IN, ANA_IN,
	DIG_IN,
	ANA_OUT, ANA_OUT, ANA_OUT, ANA_OUT,
	DIG_IN
};
static int
normal_meter_get(struct snd_bebob *bebob, u32 *buf, unsigned int size)
{
	struct snd_bebob_meter_spec *spec = bebob->spec->meter;
	unsigned int c, channels;
	int err;

	channels = spec->num * 2;
	if (size < channels * sizeof(u32))
		return -EINVAL;

	err = get_meter(bebob, (void *)buf, size);
	if (err < 0)
		goto end;

	for (c = 0; c < channels; c++)
		be32_to_cpus(&buf[c]);

	/* swap stream channels because inverted */
	if (spec->labels == solo_meter_labels) {
		swap(buf[4], buf[6]);
		swap(buf[5], buf[7]);
	}
end:
	return err;
}

/* for special customized devices */
static struct snd_bebob_rate_spec special_rate_spec = {
	.get	= &special_get_rate,
	.set	= &special_set_rate,
};
static struct snd_bebob_clock_spec special_clk_spec = {
	.num	= ARRAY_SIZE(special_clk_labels),
	.labels	= special_clk_labels,
	.get	= &special_clk_get,
};
static struct snd_bebob_meter_spec special_meter_spec = {
	.num	= ARRAY_SIZE(special_meter_labels),
	.labels	= special_meter_labels,
	.get	= &special_meter_get
};
struct snd_bebob_spec maudio_special_spec = {
	.clock	= &special_clk_spec,
	.rate	= &special_rate_spec,
	.meter	= &special_meter_spec
};

/* Firewire 410 specification */
static struct snd_bebob_rate_spec usual_rate_spec = {
	.get	= &snd_bebob_stream_get_rate,
	.set	= &snd_bebob_stream_set_rate,
};
static struct snd_bebob_meter_spec fw410_meter_spec = {
	.num	= ARRAY_SIZE(fw410_meter_labels),
	.labels	= fw410_meter_labels,
	.get	= &normal_meter_get
};
struct snd_bebob_spec maudio_fw410_spec = {
	.clock	= NULL,
	.rate	= &usual_rate_spec,
	.meter	= &fw410_meter_spec
};

/* Firewire Audiophile specification */
static struct snd_bebob_meter_spec audiophile_meter_spec = {
	.num	= ARRAY_SIZE(audiophile_meter_labels),
	.labels	= audiophile_meter_labels,
	.get	= &normal_meter_get
};
struct snd_bebob_spec maudio_audiophile_spec = {
	.clock	= NULL,
	.rate	= &usual_rate_spec,
	.meter	= &audiophile_meter_spec
};

/* Firewire Solo specification */
static struct snd_bebob_meter_spec solo_meter_spec = {
	.num	= ARRAY_SIZE(solo_meter_labels),
	.labels	= solo_meter_labels,
	.get	= &normal_meter_get
};
struct snd_bebob_spec maudio_solo_spec = {
	.clock	= NULL,
	.rate	= &usual_rate_spec,
	.meter	= &solo_meter_spec
};

/* Ozonic specification */
static struct snd_bebob_meter_spec ozonic_meter_spec = {
	.num	= ARRAY_SIZE(ozonic_meter_labels),
	.labels	= ozonic_meter_labels,
	.get	= &normal_meter_get
};
struct snd_bebob_spec maudio_ozonic_spec = {
	.clock	= NULL,
	.rate	= &usual_rate_spec,
	.meter	= &ozonic_meter_spec
};

/* NRV10 specification */
static struct snd_bebob_meter_spec nrv10_meter_spec = {
	.num	= ARRAY_SIZE(nrv10_meter_labels),
	.labels	= nrv10_meter_labels,
	.get	= &normal_meter_get
};
struct snd_bebob_spec maudio_nrv10_spec = {
	.clock	= NULL,
	.rate	= &usual_rate_spec,
	.meter	= &nrv10_meter_spec
};
