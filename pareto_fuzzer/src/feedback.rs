use super::sim;
use sim::Score;

use std::borrow::Cow;

// Import core LibAFL and LibAFL-bolts utilities
use libafl_bolts::Named;

// Import monitoring utilities for CLI and TUI (text user interface)
#[cfg(feature = "tui")]
use libafl::monitors::tui::TuiMonitor;

use libafl::state::HasSolutions;
use libafl::{
    executors::ExitKind,
    feedbacks::{Feedback, StateInitializer},
    observers::{ObserversTuple, RefCellValueObserver},
    state::HasCorpus,
    HasMetadata,
};
use libafl_bolts::tuples::{Handle, Handled, MatchNameRef};
use std::collections::HashSet;

pub struct NewScoreFeedback<'a> {
    observer_ref: Handle<RefCellValueObserver<'a, Score>>,
    seen_scores: HashSet<Score>,
}

impl<'a> NewScoreFeedback<'a> {
    pub fn new(observer_ref: &RefCellValueObserver<'a, Score>) -> Self {
        Self {
            observer_ref: observer_ref.handle(),
            seen_scores: HashSet::new(),
        }
    }
}

impl<S> StateInitializer<S> for NewScoreFeedback<'_> {}

impl<EM, I, OT, S> Feedback<EM, I, OT, S> for NewScoreFeedback<'_>
where
    OT: ObserversTuple<I, S>,
    S: HasSolutions<I> + HasCorpus<I> + HasMetadata,
{
    fn is_interesting(
        &mut self,
        _state: &mut S,
        _manager: &mut EM,
        _input: &I,
        observers: &OT,
        _exit_kind: &ExitKind,
    ) -> Result<bool, libafl::Error> {
        let observer = observers.get(&self.observer_ref).unwrap();
        let score = observer.get_ref();
        if score.validated && self.seen_scores.insert(score.clone()) {
            Ok(true)
        } else {
            Ok(false)
        }
    }
}

impl Named for NewScoreFeedback<'_> {
    fn name(&self) -> &Cow<'static, str> {
        static NAME: Cow<'static, str> = Cow::Borrowed("NewScoreFeedback");
        &NAME
    }
}


pub struct ParetoFeedback<'a> {
    observer_ref: Handle<RefCellValueObserver<'a, Score>>,
    pareto_front: HashSet<Score>,
}

impl<'a> ParetoFeedback<'a> {
    pub fn new(observer_ref: &RefCellValueObserver<'a, Score>) -> Self {
        Self {
            observer_ref: observer_ref.handle(),
            pareto_front: HashSet::new(),
        }
    }
}

impl<S> StateInitializer<S> for ParetoFeedback<'_> {}

impl<EM, I, OT, S> Feedback<EM, I, OT, S> for ParetoFeedback<'_>
where
    OT: ObserversTuple<I, S>,
    S: HasSolutions<I> + HasCorpus<I> + HasMetadata,
{
    fn is_interesting(
        &mut self,
        _state: &mut S,
        _manager: &mut EM,
        _input: &I,
        observers: &OT,
        _exit_kind: &ExitKind,
    ) -> Result<bool, libafl::Error> {
        let observer = observers.get(&self.observer_ref).unwrap();
        let score = observer.get_ref();
        if !score.validated {
            return Ok(false);
        }
        // If any existing Pareto score dominates this one, it's not interesting
        if self.pareto_front.iter().any(|s| score.is_dominated_by(s)) {
            return Ok(false);
        }
        // Remove all scores that are dominated by the new score
        self.pareto_front.retain(|s| !s.is_dominated_by(&score));
        // Insert the new score (now on the frontier)
        let was_new = self.pareto_front.insert(score.clone());
        Ok(was_new)
    }
}

impl Named for ParetoFeedback<'_> {
    fn name(&self) -> &Cow<'static, str> {
        static NAME: Cow<'static, str> = Cow::Borrowed("ParetoFeedback");
        &NAME
    }
}
