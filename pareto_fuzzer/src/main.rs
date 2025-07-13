mod feedback;
mod sim;

use sim::Score;

use std::{cell::RefCell, path::PathBuf};

// Import monitoring utilities for CLI and TUI (text user interface)
#[cfg(feature = "tui")]
use libafl::monitors::tui::TuiMonitor;
#[cfg(not(feature = "tui"))]
use libafl::monitors::SimpleMonitor;

use crate::feedback::{NewScoreFeedback, ParetoFeedback};
use libafl::{
    corpus::{Corpus, InMemoryCorpus, OnDiskCorpus, Testcase},
    events::SimpleEventManager,
    executors::{ExitKind, InProcessExecutor},
    fuzzer::StdFuzzer,
    generators::RandPrintablesGenerator,
    inputs::BytesInput,
    mutators::{havoc_mutations, HavocScheduledMutator},
    nonzero,
    observers::RefCellValueObserver,
    schedulers::QueueScheduler,
    stages::mutational::StdMutationalStage,
    state::{HasCorpus, StdState},
    Fuzzer,
};
use libafl_bolts::{current_nanos, ownedref::OwnedRef, rands::StdRand, tuples::tuple_list};

pub fn main() {
    // Initialize the logger for debug output
    env_logger::init();

    let score = RefCell::new(Score {
        cycles: u64::MAX,
        nodes: u64::MAX,
        instructions: u64::MAX,
        validated: false,
    });

    // Pass Box<Score> to the observer
    let observer = RefCellValueObserver::new("score", OwnedRef::Ref(&score));
    let mut feedback = NewScoreFeedback::new(&observer);
    let mut objective = ParetoFeedback::new(&observer);

    // Harness function: receives observer and updates the value
    let mut harness = |input: &BytesInput| {
        let target = input.as_ref();
        let input_str = String::from_utf8_lossy(target);
        score.replace(sim::sim(&input_str));
        ExitKind::Ok
    };

    // Create the fuzzer state:
    // - Random number generator seeded with current time
    // - In-memory corpus for interesting testcases
    // - On-disk corpus for crashes
    // - Feedback and empty state for auxiliary data
    let mut state = StdState::new(
        StdRand::with_seed(current_nanos()),
        InMemoryCorpus::new(),
        OnDiskCorpus::new(PathBuf::from("./results")).unwrap(),
        &mut feedback,
        &mut objective,
    )
    .unwrap();

    // Seed the corpus with a known-good input ("seed")
    // Wrap the BytesInput in a Testcase as required by the corpus
    let seed = BytesInput::new(b"seed".to_vec());
    state.corpus_mut().add(Testcase::new(seed)).unwrap();

    // Set up the monitor for output (CLI or TUI depending on feature)
    #[cfg(not(feature = "tui"))]
    let mon = SimpleMonitor::new(|s| println!("{s}"));
    #[cfg(feature = "tui")]
    let mon = TuiMonitor::builder()
        .title("Sim Fuzzer")
        .enhanced_graphics(false)
        .build();

    // Event manager handles communication between fuzzer and monitor
    let mut mgr = SimpleEventManager::new(mon);
    // Scheduler determines the order in which testcases are fuzzed
    let scheduler = QueueScheduler::new();
    // Create the fuzzer with the scheduler, feedback, and objective
    let mut fuzzer = StdFuzzer::new(scheduler, feedback, objective);
    // Executor runs the harness in-process with the observer
    let mut executor = InProcessExecutor::new(
        &mut harness,
        tuple_list!(observer),
        &mut fuzzer,
        &mut state,
        &mut mgr,
    )
    .expect("Failed to create the Executor");
    // Generator for initial random printable inputs (length 32)
    let mut generator = RandPrintablesGenerator::new(nonzero!(16));
    // Generate 8 initial random inputs to populate the corpus
    state
        .generate_initial_inputs(&mut fuzzer, &mut executor, &mut generator, &mut mgr, 8)
        .expect("Failed to generate the initial corpus");
    // Set up the mutator with default havoc mutations
    let mutator = HavocScheduledMutator::new(havoc_mutations());
    // Create the mutational stage (single stage in this fuzzer)
    let mut stages = tuple_list!(StdMutationalStage::new(mutator));
    // Start the fuzzing loop: repeatedly mutate, execute, and collect feedback
    fuzzer
        .fuzz_loop(&mut stages, &mut executor, &mut state, &mut mgr)
        .expect("Error in the fuzzing loop");
}
