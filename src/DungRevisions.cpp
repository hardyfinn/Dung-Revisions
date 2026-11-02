// DungRevisions.cpp
// A clock divider feeding a bidirectional shift register, with a display
// that shows the register, its voltages and which divisions reach each bus.
// The logic is in RungEngine.hpp; this file connects it to the panel.
#include "plugin.hpp"
#include "RungEngine.hpp"
#include <functional>

using namespace dr;

static const char* RANGE8_NAMES[] = {"0 to 5 V", "0 to 8 V", "0 to 10 V", "-5 to +5 V"};
static const char* RANGE3_NAMES[] = {"0 to 5 V", "0 to 8 V", "0 to 10 V", "-5 to +5 V", "1 semitone per step (0 to 7/12 V)", "1 octave per step (0 to 7 V)"};

static float scaleDac(int v, int maxv, int mode) {
	float f = (float) v / maxv;
	switch (mode) {
		case 0: return f * 5.f;
		case 1: return f * 8.f;
		case 2: return f * 10.f;
		case 3: return f * 10.f - 5.f;
		case 4: return v / 12.f;
		case 5: return (float) v;
	}
	return f * 8.f;
}

// A port whose tooltip says what it is doing right now
struct LivePortInfo : PortInfo {
	std::function<std::string()> live;
	std::string getDescription() override {
		std::string d = PortInfo::getDescription();
		if (live) {
			std::string l = live();
			if (!l.empty()) d = d.empty() ? l : l + "\n" + d;
		}
		return d;
	}
};

struct LengthQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		int i = clamp((int) std::round(getValue()), 0, NUM_LENGTHS - 1);
		return string::f("%d steps", LENGTHS[i]);
	}
};

struct ChanceQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float c = getValue();
		if (c >= 0.999f) return "100% loop (pattern locked)";
		if (c <= 0.001f) return "0% loop: every step is Data XOR loop";
		return string::f("%.0f%% loop / %.0f%% new", c * 100.f, (1.f - c) * 100.f);
	}
};

struct DungRevisions : Module {
	enum ParamId {
		ENUMS(ROUTE_PARAM, NUM_DIVS),  // /2 ... /8
		LENGTH_PARAM, CHANCE_PARAM, DIR_PARAM, WRITE_PARAM,
		PARAMS_LEN
	};
	enum InputId { CLOCK_INPUT, RESET_INPUT, DATA_INPUT, CHANCE_INPUT, DIR_INPUT, LENGTH_INPUT, INPUTS_LEN };
	enum OutputId {
		ENUMS(DIV_OUTPUT, NUM_DIVS),
		BUS1_OUTPUT, BUS2_OUTPUT, NOISE_OUTPUT, BIT1_OUTPUT, BIT3_OUTPUT, BIT8_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(LENGTH_LIGHT, NUM_LENGTHS), ENUMS(DIV_LIGHT, NUM_DIVS),
		BUS1_LIGHT, BUS2_LIGHT, BIT1_LIGHT, DIR_LIGHT,
		LIGHTS_LEN
	};

	RungEngine e;
	// options (right-click menu)
	int range8 = 1, range3 = 1;
	int gateLevel = 1;       // 0: 5 V, 1: 10 V
	bool dataNoise = false;  // unpatched Data reads the internal noise
	bool dirFlip = false;    // Direction jack: false = held high reverses, true = each trigger flips
	bool resetClears = false;
	bool writeSprings = true;

	dsp::SchmittTrigger clockTrig, resetTrig, dirTrig;
	bool dirFlipState = false;
	float divFlash[NUM_DIVS] = {}, busFlash[2] = {};
	float noise = 0.f;
	float chanceNow = 1.f;
	dsp::ClockDivider lightDivider;

	DungRevisions() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int i = 0; i < NUM_DIVS; i++) {
			int def = i == 1 ? 0 : i == 2 ? 2 : 1;  // /3 to Bus 2, /4 to Bus 1
			configSwitch(ROUTE_PARAM + i, 0.f, 2.f, (float) def, string::f("/%d goes to", i + 2), {"Bus 2", "Off", "Bus 1"});
		}
		ParamQuantity* lq = configParam<LengthQuantity>(LENGTH_PARAM, 0.f, 7.f, 6.f, "Length (loop point)");
		lq->snapEnabled = true;
		lq->description = "How many stages loop round. The Length jack adds 1 V per position.";
		ParamQuantity* cq = configParam<ChanceQuantity>(CHANCE_PARAM, 0.f, 1.f, 1.f, "Chance");
		cq->description = "Clockwise: the loop-point bit comes back and the pattern locks.\nAnticlockwise: Data XOR the loop-point bit goes in.\nThe Chance jack adds 10% per volt.";
		configSwitch(DIR_PARAM, 0.f, 1.f, 0.f, "Direction", {"Forward", "Reverse"});
		configSwitch(WRITE_PARAM, 0.f, 2.f, 1.f, "Write", {"Low: writes a 0 at the next step", "Off", "High: writes a 1 at the next step"});

		auto in = [&](int id, const char* name, std::function<std::string()> f) {
			LivePortInfo* p = configInput<LivePortInfo>(id, name);
			p->live = f;
		};
		auto out = [&](int id, const char* name, std::function<std::string()> f) {
			LivePortInfo* p = configOutput<LivePortInfo>(id, name);
			p->live = f;
		};
		in(CLOCK_INPUT, "Clock", [=]() {
			if (!inputs[CLOCK_INPUT].isConnected()) return std::string("Nothing patched: nothing moves until a clock arrives");
			return string::f("%u clocks so far (count %u since reset)", (unsigned) e.clocks, (unsigned) e.count);
		});
		in(RESET_INPUT, "Reset", [=]() {
			return std::string(resetClears ? "Restarts the divisions and clears the register" : "Restarts the divisions: the next clock fires all of them");
		});
		in(DATA_INPUT, "Data", [=]() {
			if (!inputs[DATA_INPUT].isConnected())
				return std::string(dataNoise ? "Unpatched: reads the internal noise (right-click option)"
				                              : "Unpatched: reads 0, so turning Chance down changes nothing yet. Try patching Noise here");
			return string::f("Reads %d now (above 0.5 V = 1). Used when Chance picks new data", e.dataBit ? 1 : 0);
		});
		in(CHANCE_INPUT, "Chance CV", [=]() { return string::f("Chance with CV: %.0f%% loop", chanceNow * 100.f); });
		in(DIR_INPUT, "Direction", [=]() {
			return string::f("%s. Register now runs %s", dirFlip ? "Each trigger flips direction" : "High = the opposite of the button",
			                 e.reverse ? "in reverse" : "forward");
		});
		in(LENGTH_INPUT, "Length CV", [=]() { return string::f("Length with CV: %d steps (1 V moves one position)", e.length()); });

		for (int i = 0; i < NUM_DIVS; i++) {
			out(DIV_OUTPUT + i, string::f("/%d", i + 2).c_str(), [=]() {
				int r = e.route[i];
				const char* to = r == ROUTE_BUS1 ? "also sent to Bus 1" : r == ROUTE_BUS2 ? "also sent to Bus 2" : "not sent to a bus";
				int k = e.clocksUntil(i);
				return string::f("Fires every %d clocks, %s. Next pulse in %d clock%s", i + 2, to, k, k == 1 ? "" : "s");
			});
		}
		out(BUS1_OUTPUT, "Bus 1", [=]() { return busWords(ROUTE_BUS1) + ". Also clocks the register"; });
		out(BUS2_OUTPUT, "Bus 2", [=]() { return busWords(ROUTE_BUS2); });
		out(NOISE_OUTPUT, "Noise", [=]() { return std::string("White noise, -5 to +5 V. Patch into Data for the classic Rungler mess"); });
		out(BIT1_OUTPUT, "1-Bit", [=]() { return string::f("Stage 1 of the register: %s now", e.bits[0] ? "high" : "low"); });
		out(BIT3_OUTPUT, "3-Bit", [=]() {
			int v = e.value3();
			return string::f("Stages 1-3 = %d of 7 = %.2f V (%s)", v, scaleDac(v, 7, range3), RANGE3_NAMES[range3]);
		});
		out(BIT8_OUTPUT, "8-Bit", [=]() {
			int v = e.value8();
			return string::f("Stages 1-8 = %d of 255 = %.2f V (%s)", v, scaleDac(v, 255, range8), RANGE8_NAMES[range8]);
		});
		lightDivider.setDivision(64);
	}

	std::string busWords(int bus) {
		std::string s;
		for (int i = 0; i < NUM_DIVS; i++) {
			if (e.route[i] != bus) continue;
			s += s.empty() ? "" : " OR ";
			s += string::f("/%d", i + 2);
		}
		if (s.empty()) return string::f("Nothing routed: flip a switch %s to send a division here", bus == ROUTE_BUS1 ? "up" : "down");
		return string::f("Bus %d = %s", bus, s.c_str());
	}

	void clearRegister() { e.clearRegister(); }
	void randomFill() { e.randomFill(); }

	void onReset() override {
		e.clearRegister();
		e.reset();
		range8 = range3 = 1;
		gateLevel = 1;
		dataNoise = dirFlip = resetClears = false;
		writeSprings = true;
		dirFlipState = false;
	}

	void process(const ProcessArgs& args) override {
		for (int i = 0; i < NUM_DIVS; i++) {
			int v = (int) std::round(params[ROUTE_PARAM + i].getValue());
			e.route[i] = v == 2 ? ROUTE_BUS1 : v == 0 ? ROUTE_BUS2 : ROUTE_OFF;
		}
		e.lengthIndex = clamp((int) std::round(params[LENGTH_PARAM].getValue() + inputs[LENGTH_INPUT].getVoltage()), 0, NUM_LENGTHS - 1);
		chanceNow = clamp(params[CHANCE_PARAM].getValue() + inputs[CHANCE_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		e.chance = chanceNow;

		// direction: button, then the jack either holds it reversed or flips it
		bool jackRev = false;
		if (inputs[DIR_INPUT].isConnected()) {
			bool rose = dirTrig.process(inputs[DIR_INPUT].getVoltage(), 0.1f, 1.f);
			if (dirFlip) {
				if (rose) dirFlipState = !dirFlipState;
				jackRev = dirFlipState;
			}
			else jackRev = dirTrig.isHigh();
		}
		e.reverse = (params[DIR_PARAM].getValue() > 0.5f) != jackRev;

		noise = (e.rng.uni() * 2.f - 1.f) * 5.f;
		float dv = inputs[DATA_INPUT].isConnected() ? inputs[DATA_INPUT].getVoltage() : (dataNoise ? noise : 0.f);
		e.dataBit = dv >= 0.5f;
		e.write = clamp((int) std::round(params[WRITE_PARAM].getValue()), 0, 2);

		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			e.reset();
			if (resetClears) e.clearRegister();
		}

		clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		bool stepped = e.setClock(clockTrig.isHigh());
		if (stepped && writeSprings && e.write != WRITE_OFF) params[WRITE_PARAM].setValue(1.f);

		// outputs
		float g = gateLevel ? 10.f : 5.f;
		for (int i = 0; i < NUM_DIVS; i++) {
			outputs[DIV_OUTPUT + i].setVoltage(e.divOut[i] ? g : 0.f);
			if (e.divOut[i]) divFlash[i] = 0.08f;
		}
		outputs[BUS1_OUTPUT].setVoltage(e.bus[0] ? g : 0.f);
		outputs[BUS2_OUTPUT].setVoltage(e.bus[1] ? g : 0.f);
		if (e.bus[0]) busFlash[0] = 0.08f;
		if (e.bus[1]) busFlash[1] = 0.08f;
		outputs[NOISE_OUTPUT].setVoltage(noise);
		outputs[BIT1_OUTPUT].setVoltage(e.bits[0] ? g : 0.f);
		outputs[BIT3_OUTPUT].setVoltage(scaleDac(e.value3(), 7, range3));
		outputs[BIT8_OUTPUT].setVoltage(scaleDac(e.value8(), 255, range8));

		// lights
		if (lightDivider.process()) {
			float dt = args.sampleTime * lightDivider.getDivision();
			for (int i = 0; i < NUM_DIVS; i++) {
				lights[DIV_LIGHT + i].setBrightness(divFlash[i] > 0.f ? 1.f : 0.f);
				divFlash[i] -= dt;
			}
			for (int b = 0; b < 2; b++) {
				lights[BUS1_LIGHT + b].setBrightness(busFlash[b] > 0.f ? 1.f : 0.f);
				busFlash[b] -= dt;
			}
			for (int i = 0; i < NUM_LENGTHS; i++) lights[LENGTH_LIGHT + i].setBrightness(i == e.lengthIndex ? 1.f : 0.f);
			lights[BIT1_LIGHT].setBrightness(e.bits[0] ? 1.f : 0.f);
			lights[DIR_LIGHT].setBrightness(e.reverse ? 1.f : 0.f);
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* b = json_array();
		for (int i = 0; i < NUM_STAGES; i++) json_array_append_new(b, json_boolean(e.bits[i]));
		json_object_set_new(root, "bits", b);
		json_object_set_new(root, "range8", json_integer(range8));
		json_object_set_new(root, "range3", json_integer(range3));
		json_object_set_new(root, "gateLevel", json_integer(gateLevel));
		json_object_set_new(root, "dataNoise", json_boolean(dataNoise));
		json_object_set_new(root, "dirFlip", json_boolean(dirFlip));
		json_object_set_new(root, "resetClears", json_boolean(resetClears));
		json_object_set_new(root, "writeSprings", json_boolean(writeSprings));
		json_object_set_new(root, "lsbFirst", json_boolean(e.lsbFirst));
		return root;
	}

	void dataFromJson(json_t* root) override {
		auto geti = [&](const char* k, int def) { json_t* j = json_object_get(root, k); return j ? (int) json_integer_value(j) : def; };
		auto getb = [&](const char* k, bool def) { json_t* j = json_object_get(root, k); return j ? json_boolean_value(j) : def; };
		json_t* b = json_object_get(root, "bits");
		if (b)
			for (int i = 0; i < NUM_STAGES && i < (int) json_array_size(b); i++) e.bits[i] = json_boolean_value(json_array_get(b, i));
		range8 = clamp(geti("range8", 1), 0, 3);
		range3 = clamp(geti("range3", 1), 0, 5);
		gateLevel = clamp(geti("gateLevel", 1), 0, 1);
		dataNoise = getb("dataNoise", false);
		dirFlip = getb("dirFlip", false);
		resetClears = getb("resetClears", false);
		writeSprings = getb("writeSprings", true);
		e.lsbFirst = getb("lsbFirst", false);
	}
};

// ---- display ---------------------------------------------------------------
static const NVGcolor C_BG = nvgRGB(0x15, 0x13, 0x11);
static const NVGcolor C_PANEL = nvgRGB(0x22, 0x1E, 0x1A);
static const NVGcolor C_INK = nvgRGB(0xEF, 0xE6, 0xD2);
static const NVGcolor C_SOFT = nvgRGB(0x8E, 0x84, 0x74);
static const NVGcolor C_DIM = nvgRGB(0x3A, 0x34, 0x2D);
static const NVGcolor C_BUS1 = nvgRGB(0x6C, 0xCB, 0x8A);
static const NVGcolor C_BUS2 = nvgRGB(0xE0, 0x7A, 0xB8);
static const NVGcolor C_CV = nvgRGB(0xE9, 0xA8, 0x4A);
static const NVGcolor C_NEW = nvgRGB(0x6F, 0xB8, 0xF0);
static const NVGcolor C_WRITE = nvgRGB(0xF0, 0x50, 0x48);

static std::shared_ptr<window::Font> displayFont() {
	return APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
}

struct RungDisplay : Widget {
	DungRevisions* module = nullptr;

	void rect(const DrawArgs& a, float x, float y, float w, float h, NVGcolor c) {
		nvgBeginPath(a.vg);
		nvgRect(a.vg, x, y, w, h);
		nvgFillColor(a.vg, c);
		nvgFill(a.vg);
	}
	void outline(const DrawArgs& a, float x, float y, float w, float h, NVGcolor c, float wd) {
		nvgBeginPath(a.vg);
		nvgRect(a.vg, x, y, w, h);
		nvgStrokeColor(a.vg, c);
		nvgStrokeWidth(a.vg, wd);
		nvgStroke(a.vg);
	}
	void line(const DrawArgs& a, float x0, float y0, float x1, float y1, NVGcolor c, float wd) {
		nvgBeginPath(a.vg);
		nvgMoveTo(a.vg, x0, y0);
		nvgLineTo(a.vg, x1, y1);
		nvgStrokeColor(a.vg, c);
		nvgStrokeWidth(a.vg, wd);
		nvgStroke(a.vg);
	}
	void text(const DrawArgs& a, float x, float y, const std::string& s, NVGcolor c, int align, float size) {
		nvgFontSize(a.vg, size);
		nvgTextAlign(a.vg, align);
		nvgFillColor(a.vg, c);
		nvgText(a.vg, x, y, s.c_str(), NULL);
	}
	void arrowHead(const DrawArgs& a, float x, float y, float dir, float s, NVGcolor c) {
		// dir +1 points right, -1 points left
		nvgBeginPath(a.vg);
		nvgMoveTo(a.vg, x, y);
		nvgLineTo(a.vg, x - dir * s, y - s * 0.6f);
		nvgLineTo(a.vg, x - dir * s, y + s * 0.6f);
		nvgClosePath(a.vg);
		nvgFillColor(a.vg, c);
		nvgFill(a.vg);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { Widget::drawLayer(args, layer); return; }
		float W = box.size.x, H = box.size.y;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, W, H, mm2px(1.2f));
		nvgFillColor(args.vg, C_BG);
		nvgFill(args.vg);
		std::shared_ptr<window::Font> font = displayFont();
		if (font && font->handle >= 0) nvgFontFaceId(args.vg, font->handle);
		const float pad = mm2px(1.6f);
		const float fs = mm2px(2.4f), fsS = mm2px(1.9f), fsT = mm2px(1.6f);
		const float x0 = pad, w = W - 2 * pad;
		const int LM = NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, RM = NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, CM = NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE;

		if (!module) {
			text(args, W / 2, H / 2, "DUNG REVISIONS", C_INK, CM, fs);
			return;
		}
		RungEngine& e = module->e;
		int L = e.length();

		// ---- register header ----
		float y = pad + fs * 0.5f;
		text(args, x0, y, "SHIFT REGISTER", C_INK, LM, fs);
		text(args, x0 + w, y, string::f("%d steps  %s", L, e.reverse ? "<< REV" : "FWD >>"), C_INK, RM, fsS);

		// ---- 16 cells ----
		float cy = y + mm2px(2.6f), ch = mm2px(5.f);
		float cw = w / NUM_STAGES;
		for (int i = 0; i < NUM_STAGES; i++) {
			float cx = x0 + i * cw;
			bool inLoop = i < L;
			NVGcolor on = inLoop ? C_INK : C_SOFT;
			rect(args, cx + 1, cy, cw - 2, ch, e.bits[i] ? on : (inLoop ? C_PANEL : nvgRGB(0x1B, 0x18, 0x15)));
			if (inLoop) outline(args, cx + 1, cy, cw - 2, ch, C_SOFT, 0.6f);
		}
		// stage numbers
		for (int i : {0, 2, 7, 15})
			text(args, x0 + (i + 0.5f) * cw, cy + ch + mm2px(1.3f), std::to_string(i + 1), C_SOFT, CM, fsT);
		// 3-bit / 8-bit brackets above the cells
		float by = cy - mm2px(0.6f);
		line(args, x0 + 1, by, x0 + 3 * cw - 1, by, C_CV, 1.2f);
		line(args, x0 + 1, by - mm2px(0.9f), x0 + 8 * cw - 1, by - mm2px(0.9f), nvgTransRGBA(C_CV, 140), 1.f);
		// loop path under the cells: from the loop point back to where bits enter
		float ly = cy + ch + mm2px(3.2f);
		float xl = x0 + (L - 0.5f) * cw, xf = x0 + 0.5f * cw;
		NVGcolor lc = e.lastSource == SRC_NEW ? C_NEW : (e.lastSource == SRC_WRITE_HIGH || e.lastSource == SRC_WRITE_LOW) ? C_WRITE : C_BUS1;
		line(args, xl, cy + ch + mm2px(0.3f), xl, ly, lc, 1.2f);
		line(args, xf, cy + ch + mm2px(0.3f), xf, ly, lc, 1.2f);
		line(args, xf, ly, xl, ly, lc, 1.2f);
		// arrow showing which end the new bit enters
		float ax = e.reverse ? xl : xf;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, ax, cy + ch + mm2px(0.2f));
		nvgLineTo(args.vg, ax - mm2px(0.8f), cy + ch + mm2px(1.3f));
		nvgLineTo(args.vg, ax + mm2px(0.8f), cy + ch + mm2px(1.3f));
		nvgClosePath(args.vg);
		nvgFillColor(args.vg, lc);
		nvgFill(args.vg);
		text(args, x0 + w, ly, "loop", lc, RM, fsT);

		// ---- what goes in next ----
		y = ly + mm2px(3.f);
		std::string nx;
		NVGcolor nc = C_INK;
		if (e.write == WRITE_HIGH) { nx = "NEXT BIT: 1 (WRITE HIGH)"; nc = C_WRITE; }
		else if (e.write == WRITE_LOW) { nx = "NEXT BIT: 0 (WRITE LOW)"; nc = C_WRITE; }
		else if (e.chance >= 0.999f) { nx = string::f("NEXT BIT: %d  loop (locked)", e.loopBit() ? 1 : 0); nc = C_BUS1; }
		else if (e.chance <= 0.001f) { nx = string::f("NEXT BIT: %d  data %d XOR loop %d", (int) (e.dataBit != e.loopBit()), (int) e.dataBit, (int) e.loopBit()); nc = C_NEW; }
		else nx = string::f("NEXT: %.0f%% loop %d / %.0f%% new %d", e.chance * 100.f, e.loopBit() ? 1 : 0, (1.f - e.chance) * 100.f, (int) (e.dataBit != e.loopBit()));
		text(args, x0, y, nx, nc, LM, fsS);

		// ---- 8-bit trace ----
		float ty = y + mm2px(3.2f), th = mm2px(20.f);
		int v8 = e.value8(), v3 = e.value3();
		text(args, x0, ty, string::f("8-BIT %3d", v8), C_CV, LM, fsS);
		text(args, x0 + w, ty, string::f("%.2f V", scaleDac(v8, 255, module->range8)), C_CV, RM, fsS);
		ty += mm2px(1.6f);
		rect(args, x0, ty, w, th, C_PANEL);
		int n = e.stepHistCount;
		float sw = w / STEP_HISTORY;
		if (n > 0) {
			nvgBeginPath(args.vg);
			for (int k = 0; k < n; k++) {
				int idx = (e.stepHistPos - n + k + STEP_HISTORY) % STEP_HISTORY;
				float xx = x0 + w - (n - k) * sw;
				float yy = ty + th - 1 - (th - 2) * e.stepHist8[idx] / 255.f;
				if (k == 0) nvgMoveTo(args.vg, xx, yy);
				else nvgLineTo(args.vg, xx, yy);
				nvgLineTo(args.vg, xx + sw, yy);
			}
			nvgStrokeColor(args.vg, C_CV);
			nvgStrokeWidth(args.vg, 1.2f);
			nvgStroke(args.vg);
		}
		else text(args, x0 + w / 2, ty + th / 2, "patch a clock and send a division to Bus 1", C_SOFT, CM, fsT);

		// ---- 3-bit bars ----
		float uy = ty + th + mm2px(3.2f), uh = mm2px(7.f);
		text(args, x0, uy, string::f("3-BIT %d", v3), C_CV, LM, fsS);
		text(args, x0 + w, uy, string::f("%.2f V", scaleDac(v3, 7, module->range3)), C_CV, RM, fsS);
		uy += mm2px(1.6f);
		rect(args, x0, uy, w, uh, C_PANEL);
		for (int k = 0; k < n; k++) {
			int idx = (e.stepHistPos - n + k + STEP_HISTORY) % STEP_HISTORY;
			float xx = x0 + w - (n - k) * sw;
			float hh = (uh - 1) * e.stepHist3[idx] / 7.f;
			if (hh > 0) rect(args, xx + 0.5f, uy + uh - hh, std::max(1.f, sw - 1.f), hh, nvgTransRGBA(C_CV, k == n - 1 ? 255 : 150));
		}

		// ---- division grid: last 24 input clocks ----
		float gy = uy + uh + mm2px(3.4f);
		text(args, x0, gy, "CLOCKS", C_INK, LM, fsS);
		text(args, x0 + w, gy, "newest >", C_SOFT, RM, fsT);
		gy += mm2px(1.8f);
		float labW = mm2px(7.f), rh = mm2px(3.6f);
		float gw = (w - labW) / CLOCK_HISTORY;
		int cn = e.clockHistCount;
		for (int r = 0; r < NUM_DIVS + 2; r++) {
			float ry = gy + r * rh;
			std::string lab;
			NVGcolor labC = C_SOFT;
			if (r < NUM_DIVS) {
				int d = NUM_DIVS - 1 - r;  // /8 at the top, like the panel
				lab = string::f("/%d", d + 2);
				int rt = e.route[d];
				labC = rt == ROUTE_BUS1 ? C_BUS1 : rt == ROUTE_BUS2 ? C_BUS2 : C_SOFT;
			}
			else lab = r == NUM_DIVS ? "BUS1" : "BUS2";
			if (r >= NUM_DIVS) labC = r == NUM_DIVS ? C_BUS1 : C_BUS2;
			text(args, x0, ry + rh / 2, lab, labC, LM, fsT);
			if (r == NUM_DIVS) line(args, x0, ry - 0.5f, x0 + w, ry - 0.5f, C_DIM, 0.8f);
			for (int k = 0; k < CLOCK_HISTORY; k++) {
				float gx = x0 + labW + k * gw;
				int age = CLOCK_HISTORY - k;  // 1 = newest
				NVGcolor cc = nvgRGB(0x1E, 0x1B, 0x18);
				if (age <= cn) {
					const ClockEvent& ev = e.clockHist[(e.clockHistPos - age + CLOCK_HISTORY) % CLOCK_HISTORY];
					if (r < NUM_DIVS) {
						int d = NUM_DIVS - 1 - r;
						if (ev.fired & (1 << d)) cc = (ev.toBus1 & (1 << d)) ? C_BUS1 : (ev.toBus2 & (1 << d)) ? C_BUS2 : C_SOFT;
					}
					else if (r == NUM_DIVS) { if (ev.bus1) cc = C_BUS1; }
					else if (ev.bus2) cc = C_BUS2;
				}
				rect(args, gx + 0.6f, ry + 0.6f, gw - 1.2f, rh - 1.2f, cc);
			}
		}

		// ---- footer ----
		float fy = gy + (NUM_DIVS + 2) * rh + mm2px(2.4f);
		text(args, x0, fy, module->busWords(ROUTE_BUS1), C_BUS1, LM, fsT);
		text(args, x0, fy + mm2px(2.3f), module->busWords(ROUTE_BUS2), C_BUS2, LM, fsT);

		Widget::drawLayer(args, layer);
	}
};

// ---- panel -----------------------------------------------------------------
// Positions in mm, matching res/make_panel.py
static const float SW_X = 6.5f, DIV_X = 16.5f;
static const float DIV_Y0 = 20.f, DIV_DY = 12.f;
static const float KNOB_X = 48.f, LEN_Y = 58.f, CHANCE_Y = 92.f;
static const float LED_R = 9.6f;

struct DungRevisionsWidget : ModuleWidget {
	DungRevisionsWidget(DungRevisions* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/DungRevisions.svg")));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		RungDisplay* d = createWidget<RungDisplay>(mm2px(Vec(64.0, 13.0)));
		d->box.size = mm2px(Vec(64.0, 104.0));
		d->module = module;
		addChild(d);

		// divisions: /8 at the top down to /2
		for (int r = 0; r < NUM_DIVS; r++) {
			int i = NUM_DIVS - 1 - r;
			float y = DIV_Y0 + r * DIV_DY;
			addParam(createParamCentered<CKSSThree>(mm2px(Vec(SW_X, y)), module, DungRevisions::ROUTE_PARAM + i));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(DIV_X, y)), module, DungRevisions::DIV_OUTPUT + i));
			addChild(createLightCentered<TinyLight<WhiteLight>>(mm2px(Vec(DIV_X + 4.4f, y - 4.4f)), module, DungRevisions::DIV_LIGHT + i));
		}
		// outputs top right
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(31.f, 20.f)), module, DungRevisions::BUS1_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(42.f, 20.f)), module, DungRevisions::BUS2_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(53.f, 20.f)), module, DungRevisions::NOISE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(31.f, 34.f)), module, DungRevisions::BIT1_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(42.f, 34.f)), module, DungRevisions::BIT3_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(53.f, 34.f)), module, DungRevisions::BIT8_OUTPUT));
		addChild(createLightCentered<TinyLight<GreenLight>>(mm2px(Vec(35.4f, 15.6f)), module, DungRevisions::BUS1_LIGHT));
		addChild(createLightCentered<TinyLight<RedLight>>(mm2px(Vec(46.4f, 15.6f)), module, DungRevisions::BUS2_LIGHT));
		addChild(createLightCentered<TinyLight<WhiteLight>>(mm2px(Vec(35.4f, 29.6f)), module, DungRevisions::BIT1_LIGHT));

		// length knob with its ring of lights
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(KNOB_X, LEN_Y)), module, DungRevisions::LENGTH_PARAM));
		for (int i = 0; i < NUM_LENGTHS; i++) {
			float a = (-150.f + 300.f * i / (NUM_LENGTHS - 1)) * (float) M_PI / 180.f;
			addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(KNOB_X + LED_R * std::sin(a), LEN_Y - LED_R * std::cos(a))), module, DungRevisions::LENGTH_LIGHT + i));
		}
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(31.f, 58.f)), module, DungRevisions::DIR_PARAM, DungRevisions::DIR_LIGHT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.f, 74.f)), module, DungRevisions::DIR_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(KNOB_X, 74.f)), module, DungRevisions::LENGTH_INPUT));

		addParam(createParamCentered<CKSSThree>(mm2px(Vec(31.f, CHANCE_Y)), module, DungRevisions::WRITE_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(KNOB_X, CHANCE_Y)), module, DungRevisions::CHANCE_PARAM));

		// bottom row inputs
		const int ins[4] = {DungRevisions::CLOCK_INPUT, DungRevisions::RESET_INPUT, DungRevisions::DATA_INPUT, DungRevisions::CHANCE_INPUT};
		const float bx[4] = {10.f, 22.f, 36.f, 48.f};
		for (int i = 0; i < 4; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(bx[i], 112.f)), module, ins[i]));
	}

	void appendContextMenu(Menu* menu) override {
		DungRevisions* m = getModule<DungRevisions>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Register"));
		menu->addChild(createMenuItem("Clear register", "", [=]() { m->clearRegister(); }));
		menu->addChild(createMenuItem("Random fill", "", [=]() { m->randomFill(); }));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Outputs"));
		menu->addChild(createIndexPtrSubmenuItem("8-Bit range", {RANGE8_NAMES[0], RANGE8_NAMES[1], RANGE8_NAMES[2], RANGE8_NAMES[3]}, &m->range8));
		menu->addChild(createIndexPtrSubmenuItem("3-Bit range",
			{RANGE3_NAMES[0], RANGE3_NAMES[1], RANGE3_NAMES[2], RANGE3_NAMES[3], RANGE3_NAMES[4], RANGE3_NAMES[5]}, &m->range3));
		menu->addChild(createIndexSubmenuItem("Bit order",
			{"Stage 1 is the biggest step (reverse-encoded)", "Stage 1 is the smallest step"},
			[=]() { return m->e.lsbFirst ? 1 : 0; },
			[=](int i) { m->e.lsbFirst = i == 1; }));
		menu->addChild(createIndexPtrSubmenuItem("Gate level", {"5 V", "10 V"}, &m->gateLevel));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Behaviour"));
		menu->addChild(createIndexSubmenuItem("Write switch",
			{"Springs back to Off after one step", "Stays where you put it"},
			[=]() { return m->writeSprings ? 0 : 1; },
			[=](int i) { m->writeSprings = i == 0; }));
		menu->addChild(createIndexSubmenuItem("Data input when unpatched",
			{"Reads 0", "Reads the internal noise"},
			[=]() { return m->dataNoise ? 1 : 0; },
			[=](int i) { m->dataNoise = i == 1; }));
		menu->addChild(createIndexSubmenuItem("Direction jack",
			{"High = opposite of the button", "Each trigger flips direction"},
			[=]() { return m->dirFlip ? 1 : 0; },
			[=](int i) { m->dirFlip = i == 1; }));
		menu->addChild(createBoolPtrMenuItem("Reset also clears the register", "", &m->resetClears));
	}
};

Model* modelDungRevisions = createModel<DungRevisions, DungRevisionsWidget>("DungRevisions");
