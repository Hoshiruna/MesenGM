#include "pch.h"
#include "Genesis/GenesisYm2612.h"
#include "Genesis/GenesisConsole.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/Audio/SoundMixer.h"
#include "Utilities/Serializer.h"

#include <cmath>

// ============================================================================
// Lookup tables (ares constants.cpp - verbatim values)
// ============================================================================
const uint8_t GenesisYm2612::_lfoDividers[8] = {
	108,
	77,
	71,
	67,
	62,
	44,
	8,
	5,
};

const uint8_t GenesisYm2612::_vibratos[8][16] = {
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 },
	{ 0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 0, 0, 0 },
	{ 0, 0, 1, 1, 2, 2, 3, 3, 3, 3, 2, 2, 1, 1, 0, 0 },
	{ 0, 0, 1, 2, 2, 2, 3, 4, 4, 3, 2, 2, 2, 1, 0, 0 },
	{ 0, 0, 2, 3, 4, 4, 5, 6, 6, 5, 4, 4, 3, 2, 0, 0 },
	{ 0, 0, 4, 6, 8, 8, 10, 12, 12, 10, 8, 8, 6, 4, 0, 0 },
	{ 0, 0, 8, 12, 16, 16, 20, 24, 24, 20, 16, 16, 12, 8, 0, 0 },
};

const uint8_t GenesisYm2612::_tremolos[4] = { 7, 3, 1, 0 };

const uint8_t GenesisYm2612::_detunes[3][8] = {
	{ 5, 6, 6, 7, 8, 8, 9, 10 },
	{ 11, 12, 13, 14, 16, 17, 18, 20 },
	{ 16, 17, 19, 20, 22, 24, 26, 28 },
};

const GenesisYm2612::EnvelopeRate GenesisYm2612::_envelopeRates[16] = {
	{ 11, { 0x00000000, 0x00000000, 0x01010101, 0x01010101 } },
	{ 10, { 0x01010101, 0x01010101, 0x01110111, 0x01110111 } },
	{ 9, { 0x01010101, 0x01011101, 0x01110111, 0x01111111 } },
	{ 8, { 0x01010101, 0x01011101, 0x01110111, 0x01111111 } },
	{ 7, { 0x01010101, 0x01011101, 0x01110111, 0x01111111 } },
	{ 6, { 0x01010101, 0x01011101, 0x01110111, 0x01111111 } },
	{ 5, { 0x01010101, 0x01011101, 0x01110111, 0x01111111 } },
	{ 4, { 0x01010101, 0x01011101, 0x01110111, 0x01111111 } },
	{ 3, { 0x01010101, 0x01011101, 0x01110111, 0x01111111 } },
	{ 2, { 0x01010101, 0x01011101, 0x01110111, 0x01111111 } },
	{ 1, { 0x01010101, 0x01011101, 0x01110111, 0x01111111 } },
	{ 0, { 0x01010101, 0x01011101, 0x01110111, 0x01111111 } },
	{ 0, { 0x11111111, 0x11121112, 0x12121212, 0x12221222 } },
	{ 0, { 0x22222222, 0x22242224, 0x24242424, 0x24442444 } },
	{ 0, { 0x44444444, 0x44484448, 0x48484848, 0x48884888 } },
	{ 0, { 0x88888888, 0x88888888, 0x88888888, 0x88888888 } },
};

// ============================================================================
// Timers (ares timer.cpp - verbatim)
// ============================================================================
void GenesisYm2612::TimerA::Run()
{
	//ares uses n10 for counter, masking to 10 bits so it wraps at 0x400.
	//With uint16_t the counter runs to 0xFFFF before triggering, 64x too slow.
	counter = (counter + 1) & 0x3FF;
	if(!counter) {
		line |= irq & enableLatch;
	}
	if(enableLatch < enable || !counter) {
		counter = period;
	}
	enableLatch = enable;
}

void GenesisYm2612::TimerB::Run()
{
	//ares uses n4 for divider, masking to 4 bits so it wraps at 0x10.
	//With uint8_t the divider runs to 0xFF before bumping counter, 16x too slow.
	divider = (divider + 1) & 0x0F;
	if(!divider) {
		counter = (counter + 1) & 0xFF;
		if(!counter) {
			line |= irq & enableLatch;
		}
	}
	if(enableLatch < enable || (!counter && !divider)) {
		counter = period; //do not reset divider on reenable
	}
	enableLatch = enable;
}

// ============================================================================
// Operator methods (ares channel.cpp - verbatim algorithm)
// ============================================================================
void GenesisYm2612::Operator::UpdateKeyState(GenesisYm2612& ym, Channel& channel)
{
	if(keyOn == keyLine) {
		return;
	}
	keyOn = keyLine;

	if(keyOn) {
		phase.value = 0;
		ssg.invert = 0;
		envelope.state = Attack;
		UpdateEnvelope(ym);
		if(envelope.rate >= 62) {
			envelope.value = 0;
		}
	} else {
		envelope.state = Release;
		UpdateEnvelope(ym);
		if(ssg.enable && ssg.attack != ssg.invert) {
			//ares uses n10 for envelope.value, so 0x200 - envelope.value auto-masks
			//to 10 bits. Without the mask, values > 0x200 produce a large uint16 that
			//corrupts SSG-EG attenuation levels (heard as noise).
			envelope.value = (0x200 - envelope.value) & 0x3FF;
		}
	}
	UpdateLevel(ym, channel);
}

void GenesisYm2612::Operator::RunEnvelope(GenesisYm2612& ym, Channel& channel)
{
	if(ym._envelope.clock & ((1 << envelope.divider) - 1)) {
		return;
	}

	if(envelope.state == Attack && envelope.value == 0) {
		envelope.state = Decay;
		UpdateEnvelope(ym);
	}

	uint32_t sustain = envelope.sustainLevel < 15 ? envelope.sustainLevel << 5 : 0x1f << 5;
	if(envelope.state == Decay && envelope.value >= sustain) {
		envelope.state = Sustain;
		UpdateEnvelope(ym);
	}

	uint32_t value = ym._envelope.clock >> envelope.divider;
	uint32_t step = envelope.steps >> ((~value & 7) << 2) & 0xf;

	if(envelope.state == Attack) {
		//ares uses n10 for envelope.value, masking to 10 bits after the add.
		//Without the mask, the ~u16(value)*step>>4 term makes value grow past
		//0x3FF instead of wrapping back, so the attack runs the wrong direction
		//(value increases instead of decreasing toward 0).
		if(envelope.rate < 62) {
			envelope.value = (envelope.value + ((~((uint16_t)(envelope.value))) * step >> 4)) & 0x3FF;
		}
	}
	if(envelope.state != Attack) {
		if(ssg.enable) {
			step = envelope.value < 0x200 ? step << 2 : 0;
		}
		envelope.value = std::min((uint32_t)envelope.value + step, (uint32_t)0x3ff);
	}

	UpdateLevel(ym, channel);
}

void GenesisYm2612::Operator::RunPhase(GenesisYm2612& ym, Channel& channel)
{
	UpdateKeyState(ym, channel);
	//ares uses n20 for phase.value, auto-masking to 20 bits on every increment.
	//Without the mask, phase.value grows without bound; the upper bits are ignored
	//by the wave() lookup's & 0x3ff, but the unbounded growth can overflow int32_t
	//in the x = modulation/2 + phase.value>>10 addition (UB) and diverge from ares.
	phase.value = (phase.value + phase.delta) & 0xFFFFF;
	if(!(ssg.enable && envelope.value >= 0x200)) {
		return;
	}

	if(!ssg.hold && !ssg.alternate) {
		phase.value = 0;
	}
	if(!(ssg.hold && ssg.invert)) {
		ssg.invert ^= ssg.alternate;
	}

	if(envelope.state == Attack) {
		//do nothing; SSG is meant to skip the attack phase
	} else if(envelope.state != Release && !ssg.hold) {
		envelope.state = Attack;
		UpdateEnvelope(ym);
		if(envelope.rate >= 62) {
			envelope.value = 0;
		}
	} else if(envelope.state == Release || (ssg.hold && ssg.attack == ssg.invert)) {
		envelope.value = 0x3ff;
	}

	UpdateLevel(ym, channel);
}

void GenesisYm2612::Operator::UpdateEnvelope(GenesisYm2612& ym)
{
	uint32_t rate = 0;

	if(envelope.state == Attack) {
		rate += (envelope.attackRate << 1);
	}
	if(envelope.state == Decay) {
		rate += (envelope.decayRate << 1);
	}
	if(envelope.state == Sustain) {
		rate += (envelope.sustainRate << 1);
	}
	if(envelope.state == Release) {
		rate += (envelope.releaseRate << 1);
	}

	rate += (keyScale >> (3 - envelope.rateScaling)) * (rate > 0);
	rate = std::min(rate, (uint32_t)63);

	auto& entry = ym._envelopeRates[rate >> 2];
	envelope.rate = rate;
	envelope.divider = entry.divider;
	envelope.steps = entry.steps[rate & 3];
}

void GenesisYm2612::Operator::UpdatePitch(GenesisYm2612& ym, Channel& channel)
{
	pitch.value = channel.mode ? pitch.reload : channel.operators[3].pitch.reload;
	octave.value = channel.mode ? octave.reload : channel.operators[3].octave.reload;

	uint32_t key = std::min(std::max((uint32_t)pitch.value, (uint32_t)0x300), (uint32_t)0x4ff);
	keyScale = (octave.value << 2) + ((key - 0x300) >> 7);

	UpdatePhase(ym, channel);
	UpdateEnvelope(ym);
}

void GenesisYm2612::Operator::UpdatePhase(GenesisYm2612& ym, Channel& channel)
{
	uint32_t tuning = detune & 3 ? _detunes[(detune & 3) - 1][keyScale & 7] >> (3 - (keyScale >> 3)) : 0;
	uint32_t lfo = ym._lfo.clock >> 2 & 0x1f;
	int32_t pm = (pitch.value * _vibratos[channel.vibrato][lfo & 15] >> 9) * (lfo > 15 ? -1 : 1);

	phase.delta = ((pitch.value + pm) << 6 >> (7 - octave.value));
	phase.delta = (!((detune >> 2) & 1) ? phase.delta + tuning : phase.delta - tuning) & 0x1ffff;
	phase.delta = (multiple ? phase.delta * multiple : phase.delta >> 1) & 0xfffff;
}

void GenesisYm2612::Operator::UpdateLevel(GenesisYm2612& ym, Channel& channel)
{
	uint32_t lfo = ym._lfo.clock & 0x40 ? ym._lfo.clock & 0x3f : ~ym._lfo.clock & 0x3f;
	uint32_t depth = _tremolos[tremoloEnable * channel.tremolo];

	bool invert = ssg.attack != ssg.invert && envelope.state != Release;
	//ares uses n10 for the local value, auto-masking the SSG inversion to 10 bits.
	//Without the mask, 0x200 - envelope.value (when value > 0x200) wraps to a large
	//uint16, producing wrong output levels (noise/harshness).
	uint16_t value = ssg.enable && invert ? (0x200 - envelope.value) & 0x3FF : 0 + envelope.value;

	outputLevel = ((totalLevel << 3) + value + (lfo << 1 >> depth)) << 3;
}

// ============================================================================
// Channel power (ares channel.cpp Channel::power - verbatim)
// ============================================================================
void GenesisYm2612::Channel::Power(GenesisYm2612& ym)
{
	leftEnable = 1;
	rightEnable = 1;
	algorithm = 0;
	feedback = 0;
	vibrato = 0;
	tremolo = 0;
	mode = 0;

	for(int i = 0; i < 4; i++) {
		auto& op = operators[i];
		op.keyOn = 0;
		op.keyLine = 0;
		op.tremoloEnable = 0;
		op.keyScale = 0;
		op.detune = 0;
		op.multiple = 0;
		op.totalLevel = 0;

		op.outputLevel = 0x1fff;
		op.output = 0;
		op.prior = 0;
		op.priorBuffer = 0;

		op.pitch.value = 0;
		op.pitch.reload = 0;
		op.pitch.latch = 0;

		op.octave.value = 0;
		op.octave.reload = 0;
		op.octave.latch = 0;

		op.phase.value = 0;
		op.phase.delta = 0;

		op.envelope.state = Release;
		op.envelope.rate = 0;
		op.envelope.divider = 11;
		op.envelope.steps = 0;
		op.envelope.value = 0x3ff;

		op.envelope.rateScaling = 0;
		op.envelope.attackRate = 0;
		op.envelope.decayRate = 0;
		op.envelope.sustainRate = 0;
		op.envelope.sustainLevel = 0;
		op.envelope.releaseRate = 1;

		op.ssg.enable = 0;
		op.ssg.attack = 0;
		op.ssg.alternate = 0;
		op.ssg.hold = 0;
		op.ssg.invert = 0;

		op.UpdatePitch(ym, *this);
		op.UpdateLevel(ym, *this);
	}
}

// ============================================================================
// Power (ares ym2612.cpp YM2612::power - verbatim sine/pow2 table generation)
// ============================================================================
void GenesisYm2612::Power()
{
	_io = {};
	_lfo = {};
	_dac = {};
	_envelope = {};
	_timerA = {};
	_timerB = {};
	for(int i = 0; i < 6; i++) {
		_channels[i].Power(*this);
	}

	const uint32_t positive = 0;
	const uint32_t negative = 1;

	for(int x = 0; x <= 0xff; x++) {
		int y = -256 * log(sin((2 * x + 1) * 3.14159265358979323846 / 1024)) / log(2) + 0.5;
		_sine[0x000 + x] = positive + (y << 1);
		_sine[0x1ff - x] = positive + (y << 1);
		_sine[0x200 + x] = negative + (y << 1);
		_sine[0x3ff - x] = negative + (y << 1);
	}

	for(int y = 0; y <= 0xff; y++) {
		int z = 1024 * pow(2, (0xff - y) / 256.0) + 0.5;
		_pow2[positive + (y << 1)] = +z;
		_pow2[negative + (y << 1)] = ~z; //not -z (ares comment)
	}
}

// ============================================================================
// Clock (ares ym2612.cpp YM2612::clock - verbatim algorithm)
// ============================================================================
void GenesisYm2612::ClockOnce()
{
	int32_t left = 0;
	int32_t right = 0;

	_timerA.Run();
	_timerB.Run();

	if(++_envelope.divider == 3) {
		_envelope.divider = 0;
		//ares uses n12 for envelope.clock, masking to 12 bits so the counter
		//cycles 1..4095 (zero skipped). With uint16_t the counter reaches 65535
		//before wrapping, making every envelope rate 16x too slow.
		_envelope.clock = (_envelope.clock + 1) & 0xFFF;
		if(!_envelope.clock) {
			_envelope.clock = 1; //12-bit counter: 1..4095 - zero skipped
		}
	}

	if(_lfo.enable && ++_lfo.divider >= _lfoDividers[_lfo.rate]) {
		_lfo.divider = 0;
		_lfo.clock++;
		for(auto& channel : _channels) {
			for(auto& op : channel.operators) {
				op.UpdatePhase(*this, channel); //vibrato
				op.UpdateLevel(*this, channel); //tremolo
			}
		}
	}

	for(auto& channel : _channels) {
		auto& op = channel.operators;

		const int32_t modMask = -(1 << 1);
		const int32_t sumMask = -(1 << 5);
		const int32_t outMask = -(1 << 5);

		auto old = [&](uint32_t n) -> int32_t { return op[n].prior & modMask; };
		auto mod = [&](uint32_t n) -> int32_t { return op[n].output & modMask; };
		auto out = [&](uint32_t n) -> int32_t { return op[n].output & sumMask; };

		auto wave = [&](uint32_t n, uint32_t modulation) -> int32_t {
			int32_t x = (modulation >> 1) + (op[n].phase.value >> 10);
			int32_t y = _sine[x & 0x3ff] + op[n].outputLevel;
			return y < 0x1a00 ? _pow2[y & 0x1ff] << 2 >> (y >> 9) : 0; //-78 dB floor
		};

		op[0].priorBuffer = op[0].prior;
		for(int n = 0; n < 4; n++) {
			op[n].prior = op[n].output;
		}

		for(auto& o : channel.operators) {
			o.RunPhase(*this, channel);
			if(_envelope.divider) {
				continue;
			}
			o.RunEnvelope(*this, channel);
		}

		int32_t feedback = modMask & ((op[0].prior + op[0].priorBuffer) >> (9 - channel.feedback));
		int32_t accumulator = 0;

		op[0].output = wave(0, feedback * (channel.feedback > 0));

		if(channel.algorithm == 0) {
			op[1].output = wave(1, mod(0));
			op[2].output = wave(2, old(1));
			op[3].output = wave(3, mod(2));
			accumulator += out(3);
		}
		if(channel.algorithm == 1) {
			op[1].output = wave(1, 0);
			op[2].output = wave(2, old(0) + old(1));
			op[3].output = wave(3, mod(2));
			accumulator += out(3);
		}
		if(channel.algorithm == 2) {
			op[1].output = wave(1, 0);
			op[2].output = wave(2, old(1));
			op[3].output = wave(3, mod(0) + mod(2));
			accumulator += out(3);
		}
		if(channel.algorithm == 3) {
			op[1].output = wave(1, mod(0));
			op[2].output = wave(2, 0);
			op[3].output = wave(3, old(1) + mod(2));
			accumulator += out(3);
		}
		if(channel.algorithm == 4) {
			op[1].output = wave(1, mod(0));
			op[2].output = wave(2, 0);
			op[3].output = wave(3, mod(2));
			accumulator += out(1) + out(3);
		}
		if(channel.algorithm == 5) {
			op[1].output = wave(1, mod(0));
			op[2].output = wave(2, old(0));
			op[3].output = wave(3, mod(0));
			accumulator += out(1) + out(2) + out(3);
		}
		if(channel.algorithm == 6) {
			op[1].output = wave(1, mod(0));
			op[2].output = wave(2, 0);
			op[3].output = wave(3, 0);
			accumulator += out(1) + out(2) + out(3);
		}
		if(channel.algorithm == 7) {
			op[1].output = wave(1, 0);
			op[2].output = wave(2, 0);
			op[3].output = wave(3, 0);
			accumulator += out(0) + out(1) + out(2) + out(3);
		}

		//sclamp<14> = clamp to 14-bit signed [-8192, 8191]
		int32_t voiceData = accumulator;
		if(voiceData > 8191) {
			voiceData = 8191;
		}
		if(voiceData < -8192) {
			voiceData = -8192;
		}
		voiceData &= outMask;

		if(_dac.enable && (&channel == &_channels[5])) {
			voiceData = ((int32_t)_dac.sample - 0x80) << 6;
		}

		//DAC output + voltage offset (crossover distortion)
		if(voiceData < 0) {
			left += channel.leftEnable * (voiceData + (1 << 5)) - (4 << 5);
			right += channel.rightEnable * (voiceData + (1 << 5)) - (4 << 5);
		} else {
			left += channel.leftEnable * (voiceData + (0 << 5)) + (4 << 5);
			right += channel.rightEnable * (voiceData + (0 << 5)) + (4 << 5);
		}
	}

	//sclamp<16>
	int16_t leftOut = (int16_t)(left > 32767 ? 32767 : left < -32768 ? -32768 :
																							 left);
	int16_t rightOut = (int16_t)(right > 32767 ? 32767 : right < -32768 ? -32768 :
																								 right);

	//Analog output stage (ares OPN2 stream): normalize to [-1,1], apply 20 Hz HPF
	//(removes DC offset / low-frequency rumble) then 2840 Hz LPF (smooths FM
	//aliasing and quantization noise), then back to int16. Without these the
	//emulation sounds harsher/noisier than real hardware.
	double ld = leftOut / 32768.0;
	double rd = rightOut / 32768.0;
	ld = _lpfLeft.Process(_hpfLeft.Process(ld));
	rd = _lpfRight.Process(_hpfRight.Process(rd));
	leftOut = (int16_t)(ld * 32768.0);
	rightOut = (int16_t)(rd * 32768.0);

	//Accumulate raw samples; consumed by MixAudio() when the primary audio
	//source (PSG) flushes its buffer.
	_samplesToPlay.push_back(leftOut);
	_samplesToPlay.push_back(rightOut);
}

// ============================================================================
// IO (ares io.cpp - verbatim register decode, port-aware address mapping)
// ============================================================================
uint8_t GenesisYm2612::ReadStatus()
{
	//ares uses bit 0 for Timer A and bit 1 for Timer B.
	//The Batman & Robin driver tests bit 1 (BIT 1,(IX+0)) for Timer B overflow.
	uint8_t status = (_timerA.line << 0) | (_timerB.line << 1);
	return status;
}

void GenesisYm2612::WriteAddress(uint8_t port, uint8_t data)
{
	//Port 0 = registers 0x000-0x0FF; port 1 = registers 0x100-0x1FF.
	//Each port has its own address latch on real hardware.
	_io.address[port] = (port ? 0x100 : 0x000) | data;
}

void GenesisYm2612::WriteData(uint8_t port, uint8_t data)
{
	uint16_t addr = _io.address[port];

	switch(addr) {
		//LFO
		case 0x022: {
			_lfo.rate = data & 0x07;
			_lfo.enable = (data >> 3) & 1;
			if(!_lfo.enable) {
				_lfo.clock = 0;
				_lfo.divider = 0;
			}
			break;
		}
		//timer A period (high)
		case 0x024: {
			_timerA.period = (_timerA.period & 0x003) | ((data & 0xFF) << 2);
			break;
		}
		//timer A period (low)
		case 0x025: {
			_timerA.period = (_timerA.period & 0x3FC) | (data & 0x03);
			break;
		}
		//timer B period
		case 0x026: {
			_timerB.period = data & 0xFF;
			if(!_timerB.enable) {
				_timerB.divider = 0;
			}
			break;
		}
		//timer control
		case 0x027: {
			_timerA.enable = data & 1;
			_timerB.enable = (data >> 1) & 1;
			_timerA.irq = (data >> 2) & 1;
			_timerB.irq = (data >> 3) & 1;
			if((data >> 4) & 1) {
				_timerA.line = 0;
			}
			if((data >> 5) & 1) {
				_timerB.line = 0;
			}
			_channels[2].mode = (data >> 6) & 0x3;
			for(int i = 0; i < 4; i++) {
				_channels[2].operators[i].UpdatePitch(*this, _channels[2]);
			}
			break;
		}
		//key on/off
		case 0x028: {
			uint32_t index = data & 0x07;
			if(index == 3 || index == 7) {
				break;
			}
			if(index >= 4) {
				index--;
			}
			_channels[index].operators[0].keyLine = (data >> 4) & 1;
			_channels[index].operators[1].keyLine = (data >> 5) & 1;
			_channels[index].operators[2].keyLine = (data >> 6) & 1;
			_channels[index].operators[3].keyLine = (data >> 7) & 1;
			break;
		}
		//DAC sample
		case 0x2a: {
			_dac.sample = data;
			break;
		}
		//DAC enable
		case 0x2b: {
			_dac.enable = (data >> 7) & 1;
			break;
		}
	}

	if((addr & 0x003) == 3) {
		return;
	}
	uint32_t voice = ((addr >> 8) & 1) * 3 + (addr & 0x3);
	uint32_t bits2_3 = (addr >> 2) & 0x3;
	//Bit-swap of 2-bit value (0,1,2,3 => 0,2,1,3). ares relies on n2 masking;
	//without & 0x3, bits2_3=2 yields index 5 and bits2_3=3 yields index 7,
	//both out of bounds for the 4-element operators[] array.
	uint32_t index = ((bits2_3 >> 1) | (bits2_3 << 1)) & 0x3;

	auto& channel = _channels[voice];
	auto& op = channel.operators[index];

	switch(addr & 0x0f0) {
		//detune, multiple
		case 0x030: {
			op.multiple = data & 0x0F;
			op.detune = (data >> 4) & 0x7;
			channel.operators[index].UpdatePhase(*this, channel);
			break;
		}
		//total level
		case 0x040: {
			op.totalLevel = data & 0x7F;
			channel.operators[index].UpdateLevel(*this, channel);
			break;
		}
		//rate scaling, attack rate
		case 0x050: {
			op.envelope.attackRate = data & 0x1F;
			op.envelope.rateScaling = (data >> 6) & 0x3;
			channel.operators[index].UpdateEnvelope(*this);
			channel.operators[index].UpdatePhase(*this, channel);
			break;
		}
		//LFO AM enable, decay rate
		case 0x060: {
			op.envelope.decayRate = data & 0x1F;
			op.tremoloEnable = (data >> 7) & 1;
			channel.operators[index].UpdateEnvelope(*this);
			channel.operators[index].UpdateLevel(*this, channel);
			break;
		}
		//sustain rate
		case 0x070: {
			op.envelope.sustainRate = data & 0x1F;
			channel.operators[index].UpdateEnvelope(*this);
			break;
		}
		//sustain level, release rate
		case 0x080: {
			op.envelope.releaseRate = ((data & 0x0F) << 1) | 1;
			op.envelope.sustainLevel = (data >> 4) & 0x0F;
			channel.operators[index].UpdateEnvelope(*this);
			break;
		}
		//SSG-EG
		case 0x090: {
			op.ssg.enable = (data >> 3) & 1;
			op.ssg.hold = op.ssg.enable ? (data & 1) : 0;
			op.ssg.alternate = op.ssg.enable ? ((data >> 1) & 1) : 0;
			op.ssg.attack = op.ssg.enable ? ((data >> 2) & 1) : 0;
			channel.operators[index].UpdateLevel(*this, channel);
			break;
		}
	}

	switch(addr & 0x0fc) {
		//pitch (low)
		case 0x0a0: {
			channel.operators[3].pitch.reload = channel.operators[3].pitch.latch | data;
			channel.operators[3].octave.reload = channel.operators[3].octave.latch;
			for(int i = 0; i < 4; i++) {
				channel.operators[i].UpdatePitch(*this, channel);
			}
			break;
		}
		//pitch (high)
		case 0x0a4: {
			//ares uses n11 for pitch.latch and n3 for octave.latch, masking the
			//data<<8 / data>>3 results. Without the masks, bits above 11/3 survive
			//and corrupt the frequency, and octave can cause a negative shift
			//(undefined behavior) in updatePhase's >> (7 - octave.value).
			channel.operators[3].pitch.latch = (data << 8) & 0x7FF;
			channel.operators[3].octave.latch = (data >> 3) & 0x7;
			break;
		}
		//per-operator pitch (low)
		case 0x0a8: {
			uint32_t idx;
			if(addr == 0x0a9) {
				idx = 0;
			} else if(addr == 0x0aa) {
				idx = 1;
			} else {
				idx = 2; //0x0a8
			}
			_channels[2].operators[idx].pitch.reload = _channels[2].operators[idx].pitch.latch | data;
			_channels[2].operators[idx].octave.reload = _channels[2].operators[idx].octave.latch;
			_channels[2].operators[idx].UpdatePitch(*this, _channels[2]);
			break;
		}
		//per-operator pitch (high)
		case 0x0ac: {
			uint32_t idx;
			if(addr == 0x0ad) {
				idx = 0;
			} else if(addr == 0x0ae) {
				idx = 1;
			} else {
				idx = 2; //0x0ac
			}
			_channels[2].operators[idx].pitch.latch = (data << 8) & 0x7FF;
			_channels[2].operators[idx].octave.latch = (data >> 3) & 0x7;
			break;
		}
		//algorithm, feedback
		case 0x0b0: {
			channel.algorithm = data & 0x07;
			channel.feedback = (data >> 3) & 0x07;
			break;
		}
		//panning, tremolo, vibrato
		case 0x0b4: {
			channel.vibrato = data & 0x07;
			channel.tremolo = (data >> 4) & 0x03;
			channel.rightEnable = (data >> 6) & 1;
			channel.leftEnable = (data >> 7) & 1;
			for(int i = 0; i < 4; i++) {
				channel.operators[i].UpdateLevel(*this, channel);
				channel.operators[i].UpdatePhase(*this, channel);
			}
			break;
		}
	}
}

// ============================================================================
// Serialization (ares serialization.cpp - converted to SV())
// ============================================================================
void GenesisYm2612::Channel::Serialize(Serializer& s)
{
	SV(leftEnable);
	SV(rightEnable);
	SV(algorithm);
	SV(feedback);
	SV(vibrato);
	SV(tremolo);
	SV(mode);
	for(int i = 0; i < 4; i++) {
		SVI(operators[i].keyOn);
		SVI(operators[i].keyLine);
		SVI(operators[i].tremoloEnable);
		SVI(operators[i].keyScale);
		SVI(operators[i].detune);
		SVI(operators[i].multiple);
		SVI(operators[i].totalLevel);
		SVI(operators[i].outputLevel);
		SVI(operators[i].output);
		SVI(operators[i].prior);
		SVI(operators[i].priorBuffer);
		SVI(operators[i].pitch.value);
		SVI(operators[i].pitch.reload);
		SVI(operators[i].pitch.latch);
		SVI(operators[i].octave.value);
		SVI(operators[i].octave.reload);
		SVI(operators[i].octave.latch);
		SVI(operators[i].phase.value);
		SVI(operators[i].phase.delta);
		SVI(operators[i].envelope.state);
		SVI(operators[i].envelope.rate);
		SVI(operators[i].envelope.divider);
		SVI(operators[i].envelope.steps);
		SVI(operators[i].envelope.value);
		SVI(operators[i].envelope.rateScaling);
		SVI(operators[i].envelope.attackRate);
		SVI(operators[i].envelope.decayRate);
		SVI(operators[i].envelope.sustainRate);
		SVI(operators[i].envelope.sustainLevel);
		SVI(operators[i].envelope.releaseRate);
		SVI(operators[i].ssg.enable);
		SVI(operators[i].ssg.attack);
		SVI(operators[i].ssg.alternate);
		SVI(operators[i].ssg.hold);
		SVI(operators[i].ssg.invert);
	}
}

void GenesisYm2612::Serialize(Serializer& s)
{
	if(s.IsSaving()) {
		Run();
	} else {
		//Clear pending samples on load; they are a transient output buffer and
		//shouldn't be restored.
		_samplesToPlay.clear();
		_prevMasterClock = _console->GetMasterClock();
		//Recompute filter coefficients (a0/b1) on load; z1 is set to 0 here but
		//will be overwritten by SV(_hpfLeft.z1) etc. below.
		ResetOutputFilters();
	}

	SV(_io.address[0]);
	SV(_io.address[1]);
	SV(_lfo.enable);
	SV(_lfo.rate);
	SV(_lfo.clock);
	SV(_lfo.divider);
	SV(_dac.enable);
	SV(_dac.sample);
	SV(_envelope.clock);
	SV(_envelope.divider);
	SV(_timerA.enable);
	SV(_timerA.enableLatch);
	SV(_timerA.irq);
	SV(_timerA.line);
	SV(_timerA.period);
	SV(_timerA.counter);
	SV(_timerB.enable);
	SV(_timerB.enableLatch);
	SV(_timerB.irq);
	SV(_timerB.line);
	SV(_timerB.period);
	SV(_timerB.counter);
	SV(_timerB.divider);

	for(int i = 0; i < 6; i++) {
		SVI(_channels[i]);
	}

	if(s.GetFormat() != SerializeFormat::Map) {
		SV(_prevMasterClock);
		//Filter state (z1) — coefficients are recomputed in ResetOutputFilters()
		//above (on load), only the transient state needs saving.
		SV(_hpfLeft.z1);
		SV(_hpfRight.z1);
		SV(_lpfLeft.z1);
		SV(_lpfRight.z1);
	}
}

// ============================================================================
// Mesen2 integration: constructor, Run, MixAudio, SetRegion
// ============================================================================

//ares OPN2 analog output stage: first-order OnePole IIR filters (nall/dsp/iir/one-pole.hpp).
//addHighPassFilter(20.0, 1) + addLowPassFilter(2840.0, 1) are applied per-sample
//before the resampler. Without these, FM aliasing/quantization noise passes through
//unchanged, and the output sounds harsher than real hardware.
void GenesisYm2612::OnePoleFilter::Reset(bool hp, double cutoffFrequency, double samplingFrequency)
{
	highPass = hp;
	z1 = 0.0;
	double x = cos(2.0 * 3.14159265358979323846 * cutoffFrequency / samplingFrequency);
	if(!hp) { //LowPass
		b1 = +2.0 - x - sqrt((+2.0 - x) * (+2.0 - x) - 1.0);
		a0 = 1.0 - b1;
	} else { //HighPass
		b1 = -2.0 - x + sqrt((-2.0 - x) * (-2.0 - x) - 1.0);
		a0 = 1.0 + b1;
	}
}

void GenesisYm2612::ResetOutputFilters()
{
	//YM2612 native sample rate = master / 1008 (~53267 Hz NTSC, ~52781 Hz PAL).
	//ares uses system.frequency() / 7.0 / 144.0 which is identical (7*144 = 1008).
	double nativeRate = (double)_console->GetMasterClockRate() / (double)MasterClocksPerSample;
	_hpfLeft.Reset(true, 20.0, nativeRate);
	_hpfRight.Reset(true, 20.0, nativeRate);
	_lpfLeft.Reset(false, 2840.0, nativeRate);
	_lpfRight.Reset(false, 2840.0, nativeRate);
}

GenesisYm2612::GenesisYm2612(Emulator* emu, GenesisConsole* console)
{
	_emu = emu;
	_console = console;
	_soundMixer = emu->GetSoundMixer();
	_settings = emu->GetSettings();

	//Register as an audio provider so MixAudio() is called when PSG (the
	//primary audio source) flushes its buffer. This mixes YM2612 output into
	//PSG's buffer rather than sending a separate PlayAudioBuffer call, which
	//caused the audio device to play PSG and YM2612 buffers sequentially
	//instead of mixed (heard as noise/static).
	_soundMixer->RegisterAudioProvider(this);

	//Sync to the console's current master clock so the first Run() call doesn't
	//try to catch up from 0 (which would produce a burst of stale samples).
	_prevMasterClock = _console->GetMasterClock();

	//Reserve capacity for ~1 frame of stereo samples (~887 YM2612 samples/frame).
	_samplesToPlay.reserve(2000 * 2);

	ResetOutputFilters();

	Power();
}

GenesisYm2612::~GenesisYm2612()
{
	_soundMixer->UnregisterAudioProvider(this);
}

void GenesisYm2612::SetRegion(ConsoleRegion region)
{
	//Recompute filter coefficients since the native sample rate changed with region.
	ResetOutputFilters();
}

void GenesisYm2612::Run()
{
	//Advance from _prevMasterClock to the console's current master clock,
	//producing one YM2612 sample per 1008 master clocks.
	uint64_t runTo = _console->GetMasterClock();
	while(_prevMasterClock + MasterClocksPerSample <= runTo) {
		_prevMasterClock += MasterClocksPerSample;
		ClockOnce();
	}
}

void GenesisYm2612::MixAudio(int16_t* out, uint32_t sampleCount, uint32_t sampleRate)
{
	//Flush any pending samples (in case Run() wasn't called recently enough).
	Run();

	//Resample YM2612 native samples (master/1008) to the target sample rate
	//and mix (add) into the output buffer. The `true` template parameter
	//enables add mode so YM2612 output is mixed with PSG output rather than
	//overwriting it.
	double nativeRate = (double)_console->GetMasterClockRate() / (double)MasterClocksPerSample;
	_resampler.SetVolume(_settings->GetGenesisConfig().Ym2612Volume / 100.0);
	_resampler.SetSampleRates(nativeRate, sampleRate);

	_resampler.Resample<true>(_samplesToPlay.data(), (uint32_t)_samplesToPlay.size() / 2, out, sampleCount, true);

	_samplesToPlay.clear();
}
