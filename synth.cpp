#include "synth.h"

#include <algorithm>
#include <cmath>

#include <SDL2/SDL.h>

// https://chromium.googlesource.com/chromium/blink/+/master/Source/modules/webaudio/PannerNode.cpp
namespace synth {

namespace {
	SDL_AudioDeviceID dev;
	std::vector<Synth*> synths;

	u32 sampleRate;
	f32 envelope;
	f32 signalDC;
	AudioPostProcessHook* bufferPostProcessHook;
	SynthPostProcessHook* synthPostProcessHook;
	// TODO: Move state to audio context
}

template<class F1, class F2, class F3>
F1 clamp(F1 v, F2 mn, F3 mx) {
	return std::max(std::min(v, (F1)mx), (F1)mn);
}

template<class F1, class F2, class F3>
decltype(F1{} + F2{}) lerp(F1 a, F2 b, F3 x) {
	return a*F3(1 - x) + b*x;
}

void audio_callback(void* ud, u8* stream, s32 len);

Synth* CreateSynth() {
	auto s = new Synth{};
	s->id = synths.size();
	s->playing = false;
	s->globalTrigger.name = "<global>";
	s->globalTrigger.state = 1;
	s->chunkPostProcess = nullptr;

	synths.push_back(s);
	return s;
}

Synth* GetSynth(u32 id) {
	if(id >= synths.size())
		return nullptr;

	return synths[id];
}

template<class... Args>
u32 CreateNode(Synth* syn, NodeType type, Args&&... vargs) {
	SynthParam args[] {vargs...};

	SynthNode node;
	node.type = type;
	node.inputTypes = 0;
	for(u32 i = 0; i < sizeof...(vargs); i++) {
		node.inputTypes |= args[i].isNode?(1u<<i):0;
		node.inputs[i] = args[i].node;
	}

	switch(type) {
		// Setting envelopes to NaN stops them from playing 
		//	before triggered.
		case NodeType::EnvelopeFade:
		case NodeType::EnvelopeADSR:
			node.phase = std::nan("");
			break;
		default: break;
	}

	syn->nodes.push_back(node);
	return syn->nodes.size()-1u;
}

u32 NewSinOscillator(Synth* syn, SynthParam freq, SynthParam phaseOffset) {
	return CreateNode(syn, NodeType::SourceSin, freq, phaseOffset);
}
u32 NewTriOscillator(Synth* syn, SynthParam freq, SynthParam phaseOffset) {
	return CreateNode(syn, NodeType::SourceTri, freq, phaseOffset);
}
u32 NewSqrOscillator(Synth* syn, SynthParam freq, SynthParam phaseOffset, SynthParam duty) {
	return CreateNode(syn, NodeType::SourceSqr, freq, phaseOffset, duty);
}
u32 NewSawOscillator(Synth* syn, SynthParam freq, SynthParam phaseOffset) {
	return CreateNode(syn, NodeType::SourceSaw, freq, phaseOffset);
}
u32 NewNoiseSource(Synth* syn) {
	return CreateNode(syn, NodeType::SourceNoise);
}
u32 NewTimeSource(Synth* syn) {
	return CreateNode(syn, NodeType::SourceTime);
}

u32 NewFadeEnvelope(Synth* syn, SynthParam duration, u32 trigger) {
	return CreateNode(syn, NodeType::EnvelopeFade, duration, trigger);
}
u32 NewADSREnvelope(Synth* syn, SynthParam attack, SynthParam decay, SynthParam sustain, SynthParam sustainlvl, 
	SynthParam release, u32 trigger) {
	return CreateNode(syn, NodeType::EnvelopeADSR, attack, decay, sustain, sustainlvl, release, trigger);
}

u32 NewLowPassEffect(Synth* syn, SynthParam input, SynthParam freq) {
	return CreateNode(syn, NodeType::EffectsLowPass, input, freq);
}
u32 NewHighPassEffect(Synth* syn, SynthParam input, SynthParam freq) {
	return CreateNode(syn, NodeType::EffectsHighPass, input, freq);
}
// u32 NewConvolutionEffect(Synth* syn, SynthParam duration, u32 trigger) {
// 	return CreateNode(syn, NodeType::EffectsConvolution, duration, trigger);
// }

u32 NewAddOperation(Synth* syn, SynthParam left, SynthParam right) {
	return CreateNode(syn, NodeType::MathAdd, left, right);
}
u32 NewSubtractOperation(Synth* syn, SynthParam left, SynthParam right) {
	return CreateNode(syn, NodeType::MathSubtract, left, right);
}
u32 NewMultiplyOperation(Synth* syn, SynthParam left, SynthParam right) {
	return CreateNode(syn, NodeType::MathMultiply, left, right);
}
u32 NewDivideOperation(Synth* syn, SynthParam left, SynthParam right) {
	return CreateNode(syn, NodeType::MathDivide, left, right);
}
u32 NewPowOperation(Synth* syn, SynthParam left, SynthParam right) {
	return CreateNode(syn, NodeType::MathPow, left, right);
}
u32 NewNegateOperation(Synth* syn, SynthParam arg) {
	return CreateNode(syn, NodeType::MathNegate, arg);
}

u32 NewSynthControl(Synth* syn, const char* name, f32 initialValue) {
	std::lock_guard<std::mutex>(syn->mutex);
	syn->controls.push_back({strdup(name), initialValue, initialValue, initialValue, 0.f});
	return CreateNode(syn, NodeType::InteractionValue, syn->controls.size()-1u);
}

u32 NewSynthTrigger(Synth* syn, const char* name) {
	std::lock_guard<std::mutex>(syn->mutex);
	syn->triggers.push_back({strdup(name), 0});
	return syn->triggers.size()-1u;
}

void SetSynthControl(Synth* syn, const char* name, f32 val, f32 lerpTime) {
	std::lock_guard<std::mutex>(syn->mutex);
	auto it = std::find_if(syn->controls.begin(), syn->controls.end(), [name](const SynthControl& ctl){
		return !strcmp(ctl.name, name);
	});
	
	if(it != syn->controls.end()) {
		it->begin = it->value;
		it->target = val;
		it->lerpTime = lerpTime;
	}
}
void TripSynthTrigger(Synth* syn, const char* name) {
	std::lock_guard<std::mutex>(syn->mutex);
	auto it = std::find_if(syn->triggers.begin(), syn->triggers.end(), [name](const SynthTrigger& ctl){
		return !strcmp(ctl.name, name);
	});

	if(it != syn->triggers.end()) {
		it->state = 1;
	}else if(!strcmp(name, "<global>")) {
		syn->globalTrigger.state = 1;
	}
}

void UpdateSynthNode(Synth* syn, u32 nodeID);

f32 EvaluateSynthNodeInput(Synth* syn, SynthNode* node, u8 input) {
	if(node->inputTypes&(1<<input)) {
		u32 nodeID = node->inputs[input].node;
		UpdateSynthNode(syn, nodeID);
		return syn->nodes[nodeID].foutput;
	}

	return node->inputs[input].value;
}

u32 EvaluateTrigger(Synth* syn, SynthNode* node, u8 input) {
	u32 nodeID = node->inputs[input].node;
	if(nodeID == ~0u) return syn->globalTrigger.state;
	return syn->triggers[nodeID].state;
}

void UpdateSynthNode(Synth* syn, u32 nodeID) {
	auto node = &syn->nodes[nodeID];
	if(node->frameID == syn->frameID) // Already updated
		return;

	node->frameID = syn->frameID;

	switch(node->type) {
		case NodeType::SourceSin: {
			f32 freq = EvaluateSynthNodeInput(syn, node, 0);
			f32 phaseOffset = EvaluateSynthNodeInput(syn, node, 1);
			node->foutput = std::sin(2.0*M_PI*(node->phase+phaseOffset));
			node->phase += freq * syn->dt;
		}	break;
		case NodeType::SourceTri: {
			f32 freq = EvaluateSynthNodeInput(syn, node, 0);
			f32 phaseOffset = EvaluateSynthNodeInput(syn, node, 1);
			auto nph = std::fmod((node->phase+phaseOffset), 1.f);
			node->foutput = (nph <= 0.5f)
				?(nph-0.25f)*4.f
				:(0.75f-nph)*4.f;
			node->phase += freq * syn->dt;
		}	break;
		case NodeType::SourceSaw: {
			f32 freq = EvaluateSynthNodeInput(syn, node, 0);
			f32 phaseOffset = EvaluateSynthNodeInput(syn, node, 1);
			node->foutput = std::fmod((node->phase+phaseOffset)*2.f, 2.f)-1.f;
			node->phase += freq * syn->dt;
		}	break;
		case NodeType::SourceSqr: {
			f32 freq = EvaluateSynthNodeInput(syn, node, 0);
			f32 phaseOffset = EvaluateSynthNodeInput(syn, node, 1);
			f32 width = EvaluateSynthNodeInput(syn, node, 2);
			width = clamp(width/2.f, 0.f, 1.f);
			auto nph = std::fmod((node->phase+phaseOffset), 1.f);
			node->foutput = (nph < width)? -1.f : 1.f;
			node->phase += freq * syn->dt;
		}	break;
		case NodeType::SourceNoise: {
			f32 val = (std::rand() %100000) / 50000.f - 0.5f;
			node->foutput = clamp(val, -1.f, 1.f);
		}	break;
		case NodeType::SourceTime: {
			node->foutput = syn->time;
		}	break;


		case NodeType::EnvelopeFade: {
			f32 duration = EvaluateSynthNodeInput(syn, node, 0);

			if(EvaluateTrigger(syn, node, 1))
				node->phase = 0.f;	

			if(std::isnan(node->phase)) {
				node->foutput = 0.f;
				break;
			}

			node->foutput = node->phase;
			node->phase = clamp(node->phase + syn->dt/duration, 0.f, 1.f);
		}	break;
		case NodeType::EnvelopeADSR: {
			f32 attack = EvaluateSynthNodeInput(syn, node, 0);
			f32 decay = EvaluateSynthNodeInput(syn, node, 1);
			f32 sustain = EvaluateSynthNodeInput(syn, node, 2);
			f32 sustainlvl = EvaluateSynthNodeInput(syn, node, 3);
			f32 release = EvaluateSynthNodeInput(syn, node, 4);

			if(EvaluateTrigger(syn, node, 5)){
				if((node->phase >= 0.0) && node->phase < (attack+decay+sustain+release))
					node->phase = node->foutput*attack;
				else
					node->phase = 0.f;
			}

			if(std::isnan(node->phase)) {
				node->foutput = 0.f;
				break;
			}

			f32 phase = node->phase;
			node->phase += syn->dt;

			if(phase < attack) {
				node->foutput = phase/attack;
				break;
			}
			phase -= attack;
			if(phase < decay) {
				node->foutput = (1.f-phase/decay*(1.f-sustainlvl));
				break;
			}
			phase -= decay;
			if(phase < sustain) {
				node->foutput = sustainlvl;
				break;
			}
			phase -= sustain;
			if(phase < release) {
				node->foutput = (1.f - phase/release)*sustainlvl;
				break;
			}

			node->foutput = 0.f;
		}	break;

		case NodeType::MathAdd: {
			f32 a = EvaluateSynthNodeInput(syn, node, 0);
			f32 b = EvaluateSynthNodeInput(syn, node, 1);
			node->foutput = a+b;
		}	break;
		case NodeType::MathSubtract: {
			f32 a = EvaluateSynthNodeInput(syn, node, 0);
			f32 b = EvaluateSynthNodeInput(syn, node, 1);
			node->foutput = a-b;
		}	break;
		case NodeType::MathMultiply: {
			f32 a = EvaluateSynthNodeInput(syn, node, 0);
			f32 b = EvaluateSynthNodeInput(syn, node, 1);
			node->foutput = a*b;
		}	break;
		case NodeType::MathDivide: {
			f32 a = EvaluateSynthNodeInput(syn, node, 0);
			f32 b = EvaluateSynthNodeInput(syn, node, 1);
			node->foutput = a/b;
		}	break;
		case NodeType::MathPow: {
			f32 a = EvaluateSynthNodeInput(syn, node, 0);
			f32 b = EvaluateSynthNodeInput(syn, node, 1);
			node->foutput = std::pow(a, b);
		}	break;
		case NodeType::MathNegate: {
			f32 a = EvaluateSynthNodeInput(syn, node, 0);
			node->foutput = -a;
		}	break;

		case NodeType::EffectsLowPass:{
			f32 i = EvaluateSynthNodeInput(syn, node, 0);
			f32 f = EvaluateSynthNodeInput(syn, node, 1);
			f32 a = syn->dt / (syn->dt + 1.f/(PI*2.f*f));
			node->foutput = lerp(node->foutput, i, a);
		}	break;
		case NodeType::EffectsHighPass:{
			f32 i = EvaluateSynthNodeInput(syn, node, 0);
			f32 f = EvaluateSynthNodeInput(syn, node, 1);
			f32 rc = 1.f/(PI*2.f*f);
			f32 a = rc / (syn->dt + rc);

			f32 result = a * (node->foutput + i - node->phase);
			node->phase = i;
			node->foutput = result;
		}	break;
		case NodeType::EffectsConvolution:{
			f32 a = EvaluateSynthNodeInput(syn, node, 0);
			node->foutput = a;
		}	break;

		case NodeType::InteractionValue: {
			u32 ctlid = node->inputs[0].node;
			node->foutput = syn->controls[ctlid].value;
		}	break;

		default: break;
	}
}

void audio_callback(void* ud, u8* stream, s32 length) {
	static std::vector<f32> intermediate;

	auto outbuffer = (f32*) stream;
	u32 buflen = (u32)length/sizeof(f32);

	std::memset(stream, 0, length);

	intermediate.resize(buflen/2);

	u32 synthID = 0;
	while(auto synth = GetSynth(synthID++)) {
		if(!synth->playing) {
			continue;
		}

		std::lock_guard<std::mutex>(synth->mutex);
		synth->dt = 1.0/sampleRate;

		for(u32 i = 0; i < intermediate.size(); i++){
			synth->frameID++;
			UpdateSynthNode(synth, synth->outputNode);
			intermediate[i] = synth->nodes[synth->outputNode].foutput;
			synth->time += synth->dt;

			for(auto& t: synth->triggers)
				t.state = 0;

			synth->globalTrigger.state = 0;

			for(auto& c: synth->controls){
				f32 span = c.target-c.begin;
				if(c.lerpTime < 1e-6 || std::abs(span) < 1e-6) {
					c.value = c.target;
					continue;
				}

				f32 a = (c.value-c.begin)/span;
				if(a < 1.f) {
					c.value += span/c.lerpTime*synth->dt;
				}else if (a > 1.f) {
					c.value = c.target;
				}
			}
		}

		f32 stereoCoefficients[2] {1.f, 1.f};

		if(synth->chunkPostProcess)
			synth->chunkPostProcess(synth, intermediate.data(), intermediate.size(), stereoCoefficients);

		if(synthPostProcessHook)
			synthPostProcessHook(synth, intermediate.data(), intermediate.size(), stereoCoefficients);

		u32 i = 0;
		for(auto v: intermediate) {
			outbuffer[i++] += v * stereoCoefficients[0];
			outbuffer[i++] += v * stereoCoefficients[1];
		}
	}

	if(bufferPostProcessHook)
		bufferPostProcessHook(outbuffer, buflen);

	constexpr f32 attackTime  = 5.f / 1000.f;
	constexpr f32 releaseTime = 200.f / 1000.f;

	f32 attack  = std::exp(-1.f / (attackTime * sampleRate));
	f32 release = std::exp(-1.f / (releaseTime * sampleRate));

	for(u32 i = 0; i < (u32)buflen; i++){
		f32 sample = outbuffer[i];
		signalDC = lerp(signalDC, sample, 0.5f/sampleRate);
		sample -= signalDC;
		f32 absSignal = std::abs(sample);

		if(absSignal > envelope) {
			envelope = lerp(envelope, absSignal, 1-attack);
		}else{
			envelope = lerp(envelope, absSignal, 1-release);
		}
		envelope = std::max(envelope, 1.f);

		f32 gain = 0.7f/envelope;
		outbuffer[i] = clamp(sample*gain, -1.f, 1.f);
	}
}

bool InitAudio(){
	SDL_AudioSpec want, have;

	std::memset(&want, 0, sizeof(want));
	want.freq = 22050;
	want.format = AUDIO_F32SYS;
	want.channels = 2;
	want.samples = 256;
	want.callback = audio_callback;

	dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if(!dev) {
		printf("Failed to open audio: %s\n", SDL_GetError());
		return false;
	}

	if(have.channels != 2) {
		printf("Failed to get stereo output\n");
		return false;		
	}

	sampleRate = have.freq;
	envelope = 1.0f;
	signalDC = 0.f;

	SDL_PauseAudioDevice(dev, 0); // start audio playing.

	return true;
}

void DeinitAudio() {
	SDL_CloseAudioDevice(dev);
}

void SetAudioPostProcessHook(AudioPostProcessHook* hook) {
	bufferPostProcessHook = hook;
}

void SetSynthPostProcessHook(SynthPostProcessHook* hook) {
	synthPostProcessHook = hook;
}

} // namespace synth