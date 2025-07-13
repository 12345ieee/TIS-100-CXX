/* *****************************************************************************
 * TIS-100-CXX
 * Copyright (c) 2024 killerbee, Andrea Stacchiotti
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 * ****************************************************************************/

#include "sim.hpp"
#include "field.hpp"
#include "levels.hpp"
#include "logger.hpp"
#include "node.hpp"
#include "parser.hpp"
#include "utils.hpp"

#include <kblib/io.h>

#include <mutex>
#include <thread>

template <typename T>
static void print_failed_test(const field& f, T&& os, bool color) {
	for (auto& i : f.inputs()) {
		os << "input " << i->x << ": ";
		write_list(os, i->inputs, nullptr, color) << '\n';
	}
	for (auto& p : f.numerics()) {
		if (not p->valid()) {
			os << "validation failure for output " << p->x;
			os << "\noutput: ";
			write_list(os, p->outputs_received, &p->outputs_expected, color);
			os << "\nexpected: ";
			write_list(os, p->outputs_expected, nullptr, color);
			os << "\n";
		}
	}
	for (auto& p : f.images()) {
		if (not p->valid()) {
			os << "validation failure for output " << p->x << "\noutput: ("
			   << p->width << ',' << p->height << ")\n"
			   << p->image_received.write_text(color) //
			   << "expected:\n"
			   << p->image_expected.write_text(color);
		}
	}
}

static void validation_success(uint8_t quiet) {
	// we use stdout here, so flush logs to avoid mangled messages in the shell
	if (not quiet) {
		log_flush();
		std::cout << print_escape(bright_blue, bold) << "validation successful"
		          << print_escape(none) << "\n";
	}
}

/// @param sc run that failed
static void validation_failure(const score& sc, int fixed, uint8_t quiet,
                               size_t cycles_limit) {
	// we use stdout here, so flush logs to avoid mangled messages in the shell
	if (quiet < 2) {
		log_flush();
		std::cout << print_escape(red) << "validation failed"
		          << print_escape(none);
		if (fixed != -1) {
			std::cout << " for fixed test " << fixed;
		}
		std::cout << " after " << sc.cycles << " cycles";
		if (sc.cycles == cycles_limit) {
			std::cout << " [timeout]";
		}
		std::cout << '\n';
	}
}

static score run(field& f, size_t cycles_limit, bool print_err) {
	score sc{};
	sc.instructions = f.instructions();
	sc.nodes = f.nodes_used();
	try {
		bool active;
		do {
			++sc.cycles;
			log_trace("step ", sc.cycles);
			log_trace_r([&] { return "Current state:\n" + f.state(); });
			active = f.step();
		} while (
		    active and sc.cycles < cycles_limit
		    and not stop_requested // testing the atomic sighandler last is
		                           // equivalent to relaxed memory order in my
		                           // tests, testing it sooner loses performance
		);

		sc.validated = true;
		for (auto& p : f.numerics()) {
			if (not p->valid()) {
				sc.validated = false;
				break;
			}
		}
		for (auto& p : f.images()) {
			if (not p->valid()) {
				sc.validated = false;
				break;
			}
		}

		if (print_err and not sc.validated) {
			log_flush();
			print_failed_test(f, std::cout, color_stdout);
		}
	} catch (const hcf_exception& e) {
		log_info("Test aborted by HCF (node ", e.x, ',', e.y, ':', e.line, ')');
		sc.validated = false;
	}

	return sc;
}

class seed_range_iterator {
 public:
	using seed_range_t = std::span<const range_t>;
	using value_type = std::uint32_t;
	using difference_type = std::ptrdiff_t;
	using iterator_concept = std::input_iterator_tag;

	seed_range_iterator() = default;
	explicit seed_range_iterator(const seed_range_t& ranges) noexcept
	    : v_end(ranges.cend())
	    , it(ranges.cbegin())
	    , cur(it->begin) {}

	std::uint32_t operator*() const noexcept { return cur; }
	seed_range_iterator& operator++() noexcept {
		++cur;
		if (cur == it->end) {
			++it;
			if (it != v_end) {
				cur = it->begin;
			}
		}
		return *this;
	}
	seed_range_iterator operator++(int) noexcept {
		auto tmp = *this;
		++*this;
		return tmp;
	}

	struct sentinel {};
	bool operator==(sentinel) const noexcept { return it == v_end; }

	static sentinel end() noexcept { return {}; }

 private:
	seed_range_t::const_iterator v_end{};
	seed_range_t::const_iterator it{};
	std::uint32_t cur{};
};

static_assert(std::input_iterator<seed_range_iterator>);
static_assert(
    std::sentinel_for<seed_range_iterator::sentinel, seed_range_iterator>);

struct run_params {
	std::size_t& total_cycles;
	bool& failure_printed;
	uint& count;
	uint& valid_count;
	std::size_t total_cycles_limit;
	std::size_t cycles_limit;
	uint cheating_success_threshold;
	std::uint8_t quiet;
	bool stats;
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-warning-option"
#pragma GCC diagnostic ignored "-Wshadow=compatible-local"
static score run_seed_ranges(level& l, field& f,
                             const std::vector<range_t>& seed_ranges,
                             run_params params, unsigned num_threads) {
	assert(not seed_ranges.empty());
	score worst{};
	seed_range_iterator seed_it(seed_ranges);
	std::mutex it_m;
	std::mutex sc_m;
	std::vector<uint> counters(num_threads);

	auto task = [](std::mutex& it_m, std::mutex& sc_m,
	               seed_range_iterator& seed_it, level& l, field f,
	               run_params params, score& worst, uint& counter) static {
		while (true) {
			std::uint32_t seed;
			{
				std::unique_lock lock(it_m);
				if (seed_it == seed_it.end()) {
					return;
				} else {
					seed = *seed_it++;
				}
			}

			auto test = l.random_test(seed);
			if (not test) {
				continue;
			}
			++counter;
			set_expected(f, std::move(*test));
			score last = run(f, params.cycles_limit, false);
			if (stop_requested) {
				return;
			}

			// none of this is hot, so it doesn't need to be parallelized
			// so it's simplest to just hold a lock the whole time
			std::unique_lock lock(sc_m);
			++params.count;
			worst.instructions = last.instructions;
			worst.nodes = last.nodes;
			params.total_cycles += last.cycles;
			if (last.validated) {
				// for random tests, only one validation is needed
				worst.validated = true;
				worst.cycles = std::max(worst.cycles, last.cycles);
				params.valid_count++;
			} else {
				if (std::exchange(params.failure_printed, true) == false) {
					log_info("Random test failed for seed: ", seed,
					         last.cycles == params.cycles_limit ? " [timeout]" : "");
					print_failed_test(f, log_info(), color_logs);
				} else {
					log_debug("Random test failed for seed: ", seed);
				}
			}
			if (not params.stats) {
				// at least K passes and at least one fail
				if (params.valid_count >= params.cheating_success_threshold
				    and params.valid_count < params.count) {
					return;
				}
			}
			if (params.total_cycles >= params.total_cycles_limit) {
				return;
			}
		}
	};
	if (f.inputs().empty()) {
		log_info("Secondary random tests skipped for invariant level");
		range_t r{0, 1};
		seed_range_iterator it2(std::span(&r, 1));
		task(it_m, sc_m, it2, l, std::move(f), params, worst, counters[0]);
	} else if (num_threads > 1) {
		std::vector<std::thread> threads;
		for (auto i : range(num_threads)) {
			threads.emplace_back(task, std::ref(it_m), std::ref(sc_m),
			                     std::ref(seed_it), std::ref(l), f.clone(), params,
			                     std::ref(worst), std::ref(counters[i]));
		}

		for (auto& t : threads) {
			t.join();
		}
		if (params.total_cycles >= params.total_cycles_limit) {
			log_info("Total cycles timeout reached, stopping tests at ",
			         params.count);
		}
		for (auto [x, i] : kblib::enumerate(counters)) {
			log_info("Thread ", i, " ran ", x, " tests");
		}
	} else {
		task(it_m, sc_m, seed_it, l, std::move(f), params, worst, counters[0]);
	}

	if (stop_requested) {
		log_warn("Stop requested");
	}

	return worst;
}
#pragma GCC diagnostic pop

score tis_sim::simulate(const std::string& solution) {
	level* l;
	std::unique_ptr<level> level_from_name;
	if (global_level) {
		l = global_level.get();
	} else if (auto filename
	           = std::filesystem::path(solution).filename().string();
	           auto maybe_id = guess_level_id(filename)) {
		level_from_name = std::make_unique<builtin_level>(*maybe_id);
		l = level_from_name.get();
		log_debug("Deduced level ", builtin_layouts[*maybe_id].segment,
		          " from filename ", kblib::quoted(filename));
	} else {
		throw std::invalid_argument{
		    concat("Impossible to determine the level ID for ",
		           kblib::quoted(filename))};
	}
	field f = l->new_field(T30_size);

	std::string code;
	if (solution == "-") {
		std::ostringstream in;
		in << std::cin.rdbuf();
		code = std::move(in).str();
	} else if (std::filesystem::is_regular_file(solution)) {
		code = kblib::try_get_file_contents(solution, std::ios::in);
	} else {
		throw std::invalid_argument{
		    concat("invalid file: ", kblib::quoted(solution))};
	}

	parse_code(f, code, T21_size);
	log_debug_r([&] { return "Layout:\n" + f.layout(); });

	score sc{};
	std::size_t total_cycles{};
	auto random_limit = cycles_limit;
	if (run_fixed) {
		sc.validated = true;
		for (uint id = 0; id < 3; ++id) {
			set_expected(f, l->static_test(id));
			score last = run(f, cycles_limit, true);
			sc.cycles = std::max(sc.cycles, last.cycles);
			sc.instructions = last.instructions;
			sc.nodes = last.nodes;
			sc.validated = sc.validated and last.validated;
			if (stop_requested) {
				log_notice("Stop requested");
				break;
			}
			total_cycles += last.cycles;
			log_info("fixed test ", id + 1, ' ',
			         last.validated ? "validated"sv : "failed"sv, " in ",
			         last.cycles, " cycles");
			if (not last.validated) {
				validation_failure(last, id + 1, quiet, cycles_limit);
				break;
			}
			// optimization: skip running the 2nd and 3rd rounds for invariant
			// levels (specifically, the image test patterns)
			if (f.inputs().empty()) {
				log_info("Secondary tests skipped for invariant level");
				break;
			}
		}
		sc.achievement = sc.validated and l->has_achievement(f, sc);
		if (sc.validated) {
			validation_success(quiet);
			auto effective_limit = static_cast<size_t>(
			    static_cast<double>(sc.cycles) * limit_multiplier);
			random_limit = std::min(cycles_limit, effective_limit);
			log_info("Setting random test timeout to ", random_limit);
		}
	}

	uint count = 0;
	uint valid_count = 0;
	if ((sc.validated or not run_fixed or show_stats) and not stop_requested
	    and not seed_ranges.empty()) {
		bool failure_printed{};
		run_params params{
		    total_cycles,
		    failure_printed,
		    count,
		    valid_count,
		    total_cycles_limit,
		    random_limit,
		    static_cast<uint>(cheat_rate * total_random_tests),
		    quiet,
		    show_stats,
		};
		auto worst = run_seed_ranges(*l, f, seed_ranges, params, num_threads);

		log_info("Random test results: ", valid_count, " passed out of ", count,
		         " total");

		if (not run_fixed) {
			sc = worst;
			if (sc.validated) {
				validation_success(quiet);
			} else {
				sc.cycles = total_cycles;
				validation_failure(sc, -1, quiet, random_limit);
			}
		}
		sc.cheat = (count == 0 or count != valid_count);
		sc.hardcoded = (valid_count <= static_cast<uint>(count * cheat_rate));
	}

	log_flush();
	if (not quiet) {
		std::cout << "score: ";
	}
	std::cout << to_string(sc);
	if (count > 0 and show_stats) {
		const auto rate = 100. * valid_count / count;
		std::cout << " PR: ";
		if (valid_count == count) {
			std::cout << print_escape(bright_blue, bold);
		} else if (rate >= 100 * cheat_rate) {
			std::cout << print_escape(yellow);
		} else {
			std::cout << print_escape(bright_red);
		}
		std::cout << rate << '%' << print_escape(none) << " (" << valid_count
		          << '/' << count << ")";
	}
	std::cout << std::endl;
	return sc;
}
