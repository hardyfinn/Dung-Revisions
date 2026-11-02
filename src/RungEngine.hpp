// RungEngine.hpp
// The logic of Dung Revisions, with no VCV Rack code so it can be tested on
// its own (see test/engine_test.cpp).
//
//   clock in -> counter -> /2 ... /8 outputs (each high only while the clock
//   is high, so the input's pulse width carries through to every output)
//   -> each division can be sent to Bus 1, Bus 2 or nowhere (3-way switch)
//   -> each bus ORs together whatever is sent to it
//   -> Bus 1 clocks a 16-stage shift register that can run either way
//   -> the bit that enters the register is chosen by Chance:
//        fully clockwise   : the bit at the loop point comes back (pattern locks)
//        fully anticlockwise: Data XOR the loop-point bit (new material)
//        in between        : a coin toss weighted by Chance picks one of the two
//   -> outputs: stage 1 as a gate, stages 1-3 as a 3-bit voltage,
//      stages 1-8 as an 8-bit voltage.
#pragma once
#include <cstdint>
#include <cstring>

namespace dr {

static const int NUM_DIVS = 7;  // /2 ... /8
static const int NUM_STAGES = 16;
static const int NUM_LENGTHS = 8;
static const int LENGTHS[NUM_LENGTHS] = {2, 3, 4, 5, 6, 7, 8, 16};
static const int STEP_HISTORY = 64;   // register steps kept for the display
static const int CLOCK_HISTORY = 24;  // input clocks kept for the display

enum Route { ROUTE_OFF = 0, ROUTE_BUS1 = 1, ROUTE_BUS2 = 2 };
enum Write { WRITE_LOW = 0, WRITE_OFF = 1, WRITE_HIGH = 2 };
// Where the last bit that entered the register came from
enum Source { SRC_NONE, SRC_LOOP, SRC_NEW, SRC_WRITE_HIGH, SRC_WRITE_LOW };

// What happened on one input clock, for the display's division grid
struct ClockEvent {
	uint8_t fired = 0;   // bit i: division /(i+2) fired
	uint8_t toBus1 = 0;  // bit i: that division was switched to Bus 1
	uint8_t toBus2 = 0;  // bit i: that division was switched to Bus 2
	bool bus1 = false, bus2 = false, stepped = false;
};

struct Rng {
	uint32_t s = 0x2545F491u;
	uint32_t next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}
	float uni() { return (next() >> 8) * (1.f / 16777216.f); }  // 0 <= x < 1
};

struct RungEngine {
	// ---- controls, set by the caller before each setClock() ----
	int route[NUM_DIVS] = {ROUTE_OFF, ROUTE_BUS2, ROUTE_BUS1, ROUTE_OFF, ROUTE_OFF, ROUTE_OFF, ROUTE_OFF};
	int lengthIndex = 6;    // 0..7 into LENGTHS
	float chance = 1.f;     // 0 = always new data, 1 = always loop
	bool dataBit = false;   // Data input, read at each register step
	int write = WRITE_OFF;
	bool reverse = false;
	bool lsbFirst = false;  // false: stage 1 is the most significant bit

	// ---- state ----
	bool bits[NUM_STAGES] = {};
	uint32_t count = 0;         // clocks since the last reset (first clock = 0)
	bool resetPending = true;   // next clock becomes count 0 (all divisions fire)
	bool clockHigh = false;
	bool divOut[NUM_DIVS] = {};
	bool bus[2] = {};
	uint32_t clocks = 0, steps = 0;  // totals, for the display
	int lastSource = SRC_NONE;
	Rng rng;

	uint8_t stepHist8[STEP_HISTORY] = {};
	uint8_t stepHist3[STEP_HISTORY] = {};
	int stepHistPos = 0, stepHistCount = 0;  // newest at stepHistPos - 1
	ClockEvent clockHist[CLOCK_HISTORY];
	int clockHistPos = 0, clockHistCount = 0;

	int length() const { return LENGTHS[lengthIndex < 0 ? 0 : lengthIndex >= NUM_LENGTHS ? NUM_LENGTHS - 1 : lengthIndex]; }
	static int divisor(int i) { return i + 2; }

	void clearRegister() { std::memset(bits, 0, sizeof(bits)); }
	void randomFill() {
		for (int i = 0; i < NUM_STAGES; i++) bits[i] = rng.next() & 1u;
	}
	void reset() { resetPending = true; }

	// Clocks until division i next fires
	int clocksUntil(int i) const {
		if (resetPending) return 1;
		int n = divisor(i);
		return n - (int) (count % n);
	}

	int value8() const {
		int v = 0;
		for (int i = 0; i < 8; i++)
			if (bits[i]) v |= 1 << (lsbFirst ? i : 7 - i);
		return v;
	}
	int value3() const {
		int v = 0;
		for (int i = 0; i < 3; i++)
			if (bits[i]) v |= 1 << (lsbFirst ? i : 2 - i);
		return v;
	}

	// The bit that the loop would bring back on the next step
	bool loopBit() const { return reverse ? bits[0] : bits[length() - 1]; }

	// Move the register one step. Returns the bit that went in.
	bool step() {
		int L = length();
		bool fb = loopBit();
		bool in;
		if (write == WRITE_HIGH) { in = true; lastSource = SRC_WRITE_HIGH; }
		else if (write == WRITE_LOW) { in = false; lastSource = SRC_WRITE_LOW; }
		else {
			bool useLoop;
			if (chance >= 0.999f) useLoop = true;
			else if (chance <= 0.001f) useLoop = false;
			else useLoop = rng.uni() < chance;
			in = useLoop ? fb : (dataBit != fb);  // != is XOR for bools
			lastSource = useLoop ? SRC_LOOP : SRC_NEW;
		}
		if (!reverse) {
			// every stage takes its lower neighbour's bit; bits past the loop
			// point keep trailing along to stage 16 and fall off the end
			for (int i = NUM_STAGES - 1; i > 0; i--) bits[i] = bits[i - 1];
			bits[0] = in;
		}
		else {
			// inside the loop each stage takes its upper neighbour's bit and the
			// new bit enters at the loop point; stages past the loop hold still
			for (int i = 0; i < L - 1; i++) bits[i] = bits[i + 1];
			bits[L - 1] = in;
		}
		steps++;
		stepHist8[stepHistPos] = (uint8_t) value8();
		stepHist3[stepHistPos] = (uint8_t) value3();
		stepHistPos = (stepHistPos + 1) % STEP_HISTORY;
		if (stepHistCount < STEP_HISTORY) stepHistCount++;
		return in;
	}

	// Call every sample with the clock input's state. Returns true when the
	// register stepped on this call.
	bool setClock(bool high) {
		bool stepped = false;
		if (high && !clockHigh) {
			if (resetPending) { count = 0; resetPending = false; }
			else count++;
			clocks++;
			ClockEvent ev;
			for (int i = 0; i < NUM_DIVS; i++) {
				bool f = count % divisor(i) == 0;
				if (f) ev.fired |= 1 << i;
				if (route[i] == ROUTE_BUS1) ev.toBus1 |= 1 << i;
				if (route[i] == ROUTE_BUS2) ev.toBus2 |= 1 << i;
			}
			ev.bus1 = (ev.fired & ev.toBus1) != 0;
			ev.bus2 = (ev.fired & ev.toBus2) != 0;
			if (ev.bus1) { step(); stepped = true; }
			ev.stepped = stepped;
			clockHist[clockHistPos] = ev;
			clockHistPos = (clockHistPos + 1) % CLOCK_HISTORY;
			if (clockHistCount < CLOCK_HISTORY) clockHistCount++;
		}
		clockHigh = high;
		// outputs follow the clock's pulse width
		bool b1 = false, b2 = false;
		for (int i = 0; i < NUM_DIVS; i++) {
			bool on = high && !resetPending && count % divisor(i) == 0;
			divOut[i] = on;
			if (on && route[i] == ROUTE_BUS1) b1 = true;
			if (on && route[i] == ROUTE_BUS2) b2 = true;
		}
		bus[0] = b1;
		bus[1] = b2;
		return stepped;
	}
};

}  // namespace dr
