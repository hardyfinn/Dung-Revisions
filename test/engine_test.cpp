// Offline tests for the Dung Revisions engine.
//   c++ -std=c++11 -O2 -o test/engine_test test/engine_test.cpp && test/engine_test
#include "../src/RungEngine.hpp"
#include <cstdio>
#include <string>

using namespace dr;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void allOff(RungEngine& e) {
	for (int i = 0; i < NUM_DIVS; i++) e.route[i] = ROUTE_OFF;
}

// One clock pulse; returns whether the register stepped
static bool pulse(RungEngine& e) {
	bool s = e.setClock(true);
	e.setClock(false);
	return s;
}

static int stage1Run(RungEngine& e, int n, char* out) {
	for (int i = 0; i < n; i++) {
		pulse(e);
		out[i] = e.bits[0] ? '1' : '0';
	}
	out[n] = 0;
	return 0;
}

int main() {
	// ---- divider counts ----
	{
		RungEngine e;
		allOff(e);
		e.route[0] = ROUTE_BUS1;  // /2
		int steps = 0, div5 = 0;
		for (int c = 0; c < 20; c++) {
			if (e.setClock(true)) steps++;
			if (e.divOut[3]) div5++;
			CHECK(e.bus[0] == e.divOut[0], "bus 1 follows /2 at clock %d", c);
			e.setClock(false);
			CHECK(!e.divOut[0] && !e.bus[0], "outputs low while clock low");
		}
		CHECK(steps == 10, "/2 on Bus 1 steps the register 10 times in 20 clocks (got %d)", steps);
		CHECK(div5 == 4, "/5 fires 4 times in 20 clocks (got %d)", div5);
	}
	// ---- OR on a bus: /2 and /4 overlap so still 10 steps; /2 and /3 give 14 ----
	{
		RungEngine e;
		allOff(e);
		e.route[0] = ROUTE_BUS1;
		e.route[2] = ROUTE_BUS1;
		int steps = 0;
		for (int c = 0; c < 20; c++) steps += pulse(e);
		CHECK(steps == 10, "/2 OR /4 = 10 steps (got %d)", steps);
		RungEngine f;
		allOff(f);
		f.route[0] = ROUTE_BUS1;
		f.route[1] = ROUTE_BUS1;
		steps = 0;
		for (int c = 0; c < 12; c++) steps += pulse(f);
		CHECK(steps == 8, "/2 OR /3 = 8 steps in 12 clocks (got %d)", steps);
	}
	// ---- Bus 2 does not clock the register ----
	{
		RungEngine e;
		allOff(e);
		e.route[0] = ROUTE_BUS2;
		int steps = 0;
		for (int c = 0; c < 10; c++) steps += pulse(e);
		CHECK(steps == 0, "Bus 2 alone never steps the register");
	}
	// ---- reset: next clock fires every division ----
	{
		RungEngine e;
		allOff(e);
		for (int c = 0; c < 5; c++) pulse(e);
		e.reset();
		e.setClock(true);
		bool all = true;
		for (int i = 0; i < NUM_DIVS; i++) all = all && e.divOut[i];
		CHECK(all, "after reset all divisions fire together");
		e.setClock(false);
		CHECK(e.clocksUntil(5) == 7, "/7 next fires in 7 clocks (got %d)", e.clocksUntil(5));
	}
	// ---- locked loop, forward and reverse ----
	{
		RungEngine r;
		r.lengthIndex = 2;  // length 4
		r.chance = 1.f;
		r.bits[0] = true;
		char seq[9];
		for (int i = 0; i < 8; i++) { r.step(); seq[i] = r.bits[0] ? '1' : '0'; }
		seq[8] = 0;
		CHECK(std::string(seq) == "00010001", "length 4 forward loop: %s", seq);
		r.reverse = true;
		for (int i = 0; i < 8; i++) { r.step(); seq[i] = r.bits[0] ? '1' : '0'; }
		CHECK(std::string(seq) == "00100010" || std::string(seq) == "01000100" || std::string(seq) == "10001000" || std::string(seq) == "00010001",
		      "length 4 reverse loop keeps one bit: %s", seq);
		int ones = 0;
		for (int i = 0; i < 4; i++) ones += r.bits[i];
		CHECK(ones == 1, "reverse loop keeps exactly one 1 in the loop");
		(void) stage1Run;
	}
	// ---- write high / low ----
	{
		RungEngine r;
		r.write = WRITE_HIGH;
		r.step();
		CHECK(r.bits[0], "write high puts a 1 into stage 1");
		CHECK(r.lastSource == SRC_WRITE_HIGH, "source recorded");
		r.write = WRITE_LOW;
		r.step();
		CHECK(!r.bits[0] && r.bits[1], "write low puts a 0 in, the 1 moves on");
	}
	// ---- chance fully CCW: Data XOR loop ----
	{
		RungEngine r;
		r.lengthIndex = 0;  // length 2
		r.chance = 0.f;
		r.dataBit = true;
		char seq[7];
		for (int i = 0; i < 6; i++) { r.step(); seq[i] = r.bits[0] ? '1' : '0'; }
		seq[6] = 0;
		// in = 1 XOR bits[1]: 1, 1, 0, 0, 1, 1
		CHECK(std::string(seq) == "110011", "data XOR loop at length 2: %s", seq);
		RungEngine q;
		q.chance = 0.f;
		q.dataBit = false;
		q.bits[7] = true;
		q.lengthIndex = 6;
		q.step();
		CHECK(q.bits[0], "data 0 XOR loop bit 1 = 1 (loops)");
	}
	// ---- in-between chance mixes ----
	{
		RungEngine r;
		r.chance = 0.5f;
		r.dataBit = true;
		int loops = 0, news = 0;
		for (int i = 0; i < 2000; i++) {
			r.step();
			if (r.lastSource == SRC_LOOP) loops++;
			if (r.lastSource == SRC_NEW) news++;
		}
		CHECK(loops > 800 && news > 800, "chance 50%% gives both (loop %d, new %d)", loops, news);
	}
	// ---- DAC encodings ----
	{
		RungEngine r;
		r.bits[0] = true;
		CHECK(r.value8() == 128 && r.value3() == 4, "stage 1 is the most significant bit (%d, %d)", r.value8(), r.value3());
		r.lsbFirst = true;
		CHECK(r.value8() == 1 && r.value3() == 1, "lsbFirst flips it");
		r.lsbFirst = false;
		for (int i = 0; i < 8; i++) r.bits[i] = true;
		CHECK(r.value8() == 255 && r.value3() == 7, "all ones");
	}
	// ---- length 16 and trailing bits ----
	{
		RungEngine r;
		r.lengthIndex = 7;
		CHECK(r.length() == 16, "index 7 = 16 steps");
		r.lengthIndex = 1;  // length 3
		r.chance = 1.f;
		r.bits[0] = true;
		for (int i = 0; i < 3; i++) r.step();
		CHECK(r.bits[0] && r.bits[3], "length 3 loop returns, and a copy trails into stage 4");
	}
	// ---- histories ----
	{
		RungEngine e;
		allOff(e);
		e.route[0] = ROUTE_BUS1;
		for (int c = 0; c < 30; c++) pulse(e);
		CHECK(e.clockHistCount == CLOCK_HISTORY, "clock history fills");
		CHECK(e.stepHistCount == 15, "15 register steps recorded (got %d)", e.stepHistCount);
		CHECK(e.clocks == 30 && e.steps == 15, "totals");
	}

	if (failures) {
		std::printf("%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("all engine checks passed\n");
	return 0;
}
