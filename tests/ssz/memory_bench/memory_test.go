// This benchmark tests the memory retention claim about value types vs pointer types.
//
// Claim: "load a mainnet beacon state (200MB+), and reference a single Validator
// entry out of it. The whole underlying state object is then locked in memory
// till that reference is removed."
//
// Result: This claim is WRONG for Go value types. When you index into a
// []Validator slice (value type), you get a COPY. The original state can be
// GC'd independently. The retention problem only exists with SUB-SLICING,
// which affects pointer types equally.
package memory_bench

import (
	"fmt"
	"runtime"
	"testing"
)

// Validator is 121 bytes — same as Ethereum's SSZ Validator.
type Validator struct {
	Pubkey                     [48]byte
	WithdrawalCredentials      [32]byte
	EffectiveBalance           uint64
	Slashed                    bool
	ActivationEligibilityEpoch uint64
	ActivationEpoch            uint64
	ExitEpoch                  uint64
	WithdrawableEpoch          uint64
}

// ---- Value type approach (flatssz) ----

type ValueState struct {
	Validators []Validator // value type — each element is inline in the backing array
}

// ---- Pointer type approach (dynamic-ssz / fastssz) ----

type PtrState struct {
	Validators []*Validator // pointer type — each element is a separate heap allocation
}

const numValidators = 1_000_000 // ~1M validators ≈ 121MB for value, more for pointer

func makeValueState() *ValueState {
	s := &ValueState{
		Validators: make([]Validator, numValidators),
	}
	for i := range s.Validators {
		s.Validators[i].EffectiveBalance = 32_000_000_000
		s.Validators[i].Pubkey[0] = byte(i)
	}
	return s
}

func makePtrState() *PtrState {
	s := &PtrState{
		Validators: make([]*Validator, numValidators),
	}
	for i := range s.Validators {
		s.Validators[i] = &Validator{
			EffectiveBalance: 32_000_000_000,
		}
		s.Validators[i].Pubkey[0] = byte(i)
	}
	return s
}

func heapMB() float64 {
	runtime.GC()
	runtime.GC()
	var m runtime.MemStats
	runtime.ReadMemStats(&m)
	return float64(m.HeapAlloc) / 1024 / 1024
}

// TestMemoryRetention_ValueType proves that extracting a single validator
// from a value-type slice does NOT retain the parent state.
func TestMemoryRetention_ValueType(t *testing.T) {
	baseline := heapMB()

	state := makeValueState()
	afterAlloc := heapMB()
	stateSize := afterAlloc - baseline
	t.Logf("Value state allocated: %.1f MB", stateSize)

	// Extract a single validator by INDEX (copy)
	v := state.Validators[42]
	_ = v.EffectiveBalance

	// Drop the state
	state = nil
	afterDrop := heapMB()
	retained := afterDrop - baseline

	t.Logf("After dropping state (kept 1 validator copy): %.1f MB retained", retained)

	if retained > stateSize*0.1 {
		t.Errorf("MEMORY RETAINED: %.1f MB (%.0f%% of state)", retained, retained/stateSize*100)
	} else {
		t.Logf("PASSED: state was GC'd, only %.1f MB retained (validator copy)", retained)
	}

	// Prevent v from being optimized away
	if v.EffectiveBalance == 0 {
		t.Fatal("unreachable")
	}
}

// TestMemoryRetention_PtrType shows pointer-type behavior for comparison.
func TestMemoryRetention_PtrType(t *testing.T) {
	baseline := heapMB()

	state := makePtrState()
	afterAlloc := heapMB()
	stateSize := afterAlloc - baseline
	t.Logf("Pointer state allocated: %.1f MB", stateSize)

	// Extract a single validator by INDEX (pointer — independent allocation)
	v := state.Validators[42]
	_ = v.EffectiveBalance

	// Drop the state
	state = nil
	afterDrop := heapMB()
	retained := afterDrop - baseline

	t.Logf("After dropping state (kept 1 validator ptr): %.1f MB retained", retained)

	if retained > stateSize*0.1 {
		t.Errorf("MEMORY RETAINED: %.1f MB (%.0f%% of state)", retained, retained/stateSize*100)
	} else {
		t.Logf("PASSED: state was GC'd, only %.1f MB retained (validator ptr)", retained)
	}

	if v.EffectiveBalance == 0 {
		t.Fatal("unreachable")
	}
}

// TestMemoryRetention_SubSlice shows that SUB-SLICING retains the backing
// array for BOTH value and pointer types.
func TestMemoryRetention_SubSlice(t *testing.T) {
	t.Run("value_subslice", func(t *testing.T) {
		baseline := heapMB()

		state := makeValueState()
		stateSize := heapMB() - baseline
		t.Logf("Value state: %.1f MB", stateSize)

		// Sub-slice shares the backing array
		sub := state.Validators[42:43]
		state = nil
		retained := heapMB() - baseline

		t.Logf("After drop (kept sub-slice): %.1f MB retained (%.0f%%)", retained, retained/stateSize*100)
		_ = sub[0].EffectiveBalance
	})

	t.Run("ptr_subslice", func(t *testing.T) {
		baseline := heapMB()

		state := makePtrState()
		stateSize := heapMB() - baseline
		t.Logf("Pointer state: %.1f MB", stateSize)

		// Sub-slice shares the backing array of pointers
		sub := state.Validators[42:43]
		state = nil
		retained := heapMB() - baseline

		t.Logf("After drop (kept sub-slice): %.1f MB retained (%.0f%%)", retained, retained/stateSize*100)
		_ = sub[0].EffectiveBalance
	})
}

// BenchmarkUnmarshal_ValueVsPtr measures allocation overhead.
func BenchmarkUnmarshal_ValueType(b *testing.B) {
	for i := 0; i < b.N; i++ {
		s := makeValueState()
		_ = s.Validators[0].EffectiveBalance
	}
}

func BenchmarkUnmarshal_PtrType(b *testing.B) {
	for i := 0; i < b.N; i++ {
		s := makePtrState()
		_ = s.Validators[0].EffectiveBalance
	}
}

// BenchmarkMemoryPerValidator shows per-validator memory overhead.
func BenchmarkMemoryPerValidator(b *testing.B) {
	b.Run("value", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			s := make([]Validator, 1000)
			s[0].EffectiveBalance = 32_000_000_000
			_ = s
		}
	})
	b.Run("ptr", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			s := make([]*Validator, 1000)
			for j := range s {
				s[j] = &Validator{EffectiveBalance: 32_000_000_000}
			}
			_ = s
		}
	})
}

// ---- Iteration benchmarks ----
// Value types are contiguous in memory (cache-friendly).
// Pointer types require pointer chasing (cache-hostile).

var sinkU64 uint64

// BenchmarkIterate_SumBalance iterates all validators and sums EffectiveBalance.
func BenchmarkIterate_SumBalance(b *testing.B) {
	valueState := makeValueState()
	ptrState := makePtrState()

	b.Run("value", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			var sum uint64
			for j := range valueState.Validators {
				sum += valueState.Validators[j].EffectiveBalance
			}
			sinkU64 = sum
		}
	})

	b.Run("ptr", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			var sum uint64
			for _, v := range ptrState.Validators {
				sum += v.EffectiveBalance
			}
			sinkU64 = sum
		}
	})
}

// BenchmarkIterate_CountActive counts validators with a non-max exit epoch,
// touching multiple fields per validator.
func BenchmarkIterate_CountActive(b *testing.B) {
	// Set some validators as exited
	valueState := makeValueState()
	ptrState := makePtrState()
	for i := 0; i < numValidators; i++ {
		if i%10 == 0 {
			valueState.Validators[i].ExitEpoch = 100
			ptrState.Validators[i].ExitEpoch = 100
		} else {
			valueState.Validators[i].ExitEpoch = ^uint64(0)
			ptrState.Validators[i].ExitEpoch = ^uint64(0)
		}
		valueState.Validators[i].ActivationEpoch = uint64(i % 1000)
		ptrState.Validators[i].ActivationEpoch = uint64(i % 1000)
	}

	b.Run("value", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			count := 0
			for j := range valueState.Validators {
				v := &valueState.Validators[j]
				if v.ActivationEpoch <= 500 && v.ExitEpoch == ^uint64(0) && !v.Slashed {
					count++
				}
			}
			sinkU64 = uint64(count)
		}
	})

	b.Run("ptr", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			count := 0
			for _, v := range ptrState.Validators {
				if v.ActivationEpoch <= 500 && v.ExitEpoch == ^uint64(0) && !v.Slashed {
					count++
				}
			}
			sinkU64 = uint64(count)
		}
	})
}

// BenchmarkIterate_ModifyBalance modifies each validator's balance in-place.
func BenchmarkIterate_ModifyBalance(b *testing.B) {
	valueState := makeValueState()
	ptrState := makePtrState()

	b.Run("value", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			for j := range valueState.Validators {
				valueState.Validators[j].EffectiveBalance += 1
			}
		}
	})

	b.Run("ptr", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			for _, v := range ptrState.Validators {
				v.EffectiveBalance += 1
			}
		}
	})
}

func init() {
	fmt.Printf("Validator struct size: %d bytes\n", 121)
	fmt.Printf("Validator count: %d\n", numValidators)
	fmt.Printf("Expected value state size: %.0f MB\n", float64(numValidators*121)/1024/1024)
}
