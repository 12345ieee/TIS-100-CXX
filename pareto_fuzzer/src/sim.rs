use serde::{Deserialize, Serialize};
use std::hash::{Hash, Hasher};
use libafl::Display;

#[derive(Serialize, Deserialize, Clone, Display)]
pub struct Score {
    pub cycles: u64,
    pub nodes: u64,
    pub instructions: u64,
    pub validated: bool,
}

impl Score {
    /// Quickly create a Score with validated = false and default values for other fields
    pub fn invalid() -> Self {
        Score {
            cycles: 0,
            nodes: 0,
            instructions: 0,
            validated: false,
        }
    }
    // Returns true if self is strictly dominated by other (all metrics worse or equal, at least one strictly worse)
    pub fn is_dominated_by(&self, other: &Self) -> bool {
        (self.cycles >= other.cycles)
            && (self.nodes >= other.nodes)
            && (self.instructions >= other.instructions)
            && ((self.cycles > other.cycles)
                || (self.nodes > other.nodes)
                || (self.instructions > other.instructions))
    }
}

impl PartialEq for Score {
    fn eq(&self, other: &Self) -> bool {
        self.cycles == other.cycles
            && self.nodes == other.nodes
            && self.instructions == other.instructions
            && self.validated == other.validated
    }
}
impl Eq for Score {}
impl Hash for Score {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.cycles.hash(state);
        self.nodes.hash(state);
        self.instructions.hash(state);
        self.validated.hash(state);
    }
}

pub fn sim(input: &str) -> Score {
    let bytes = input.as_bytes();
    if bytes.is_empty() {
        return Score::invalid();
    }
    let sum: u32 = bytes.iter().map(|&b| b as u32).sum();
    if sum % 5 == 0 {
        return Score::invalid();
    }
    let mut c = 0u64;
    let mut n = 0u64;
    let mut i = 0u64;
    for (idx, &byte) in bytes.iter().enumerate() {
        match idx % 3 {
            0 => c = c.wrapping_mul(31).wrapping_add(byte as u64),
            1 => n = n.wrapping_mul(131).wrapping_add(byte as u64),
            2 => i = i.wrapping_mul(17).wrapping_add(byte as u64),
            _ => unreachable!(),
        }
    }
    let c = c % 1_000 + 1;
    let n = n % 20 + 1;
    let i = i % 200 + 1;
    return Score {
        cycles: c,
        nodes: n,
        instructions: i,
        validated: true,
    };
}
