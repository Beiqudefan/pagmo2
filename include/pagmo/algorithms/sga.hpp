/* Copyright 2017 PaGMO development team

This file is part of the PaGMO library.

The PaGMO library is free software; you can redistribute it and/or modify
it under the terms of either:

  * the GNU Lesser General Public License as published by the Free
    Software Foundation; either version 3 of the License, or (at your
    option) any later version.

or

  * the GNU General Public License as published by the Free Software
    Foundation; either version 3 of the License, or (at your option) any
    later version.

or both in parallel, as here.

The PaGMO library is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received copies of the GNU General Public License and the
GNU Lesser General Public License along with the PaGMO library.  If not,
see https://www.gnu.org/licenses/. */

#ifndef PAGMO_ALGORITHMS_SGA_HPP
#define PAGMO_ALGORITHMS_SGA_HPP

#include <algorithm> // std::sort, std::all_of, std::copy
#include <boost/bimap.hpp>
#include <iomanip>
#include <numeric> // std::iota
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "../algorithm.hpp"
#include "../detail/custom_comparisons.hpp"
#include "../exceptions.hpp"
#include "../io.hpp"
#include "../population.hpp"
#include "../rng.hpp"
#include "../utils/generic.hpp"

namespace pagmo
{
namespace detail
{
// Usual template trick to have static members in header only libraries
// see: http://stackoverflow.com/questions/18860895/how-to-initialize-static-members-in-the-header
// All this scaffolding is to establish a one to one correspondance between enums and genetic operator types
// represented as strings.
template <typename = void>
struct sga_statics {
    enum class selection { TOURNAMENT, TRUNCATED };
    enum class crossover { EXPONENTIAL, BINOMIAL, SINGLE, SBX };
    enum class mutation { GAUSSIAN, UNIFORM, POLYNOMIAL };
    using selection_map_t = boost::bimap<std::string, selection>;
    using crossover_map_t = boost::bimap<std::string, crossover>;
    using mutation_map_t = boost::bimap<std::string, mutation>;
    const static selection_map_t m_selection_map;
    const static crossover_map_t m_crossover_map;
    const static mutation_map_t m_mutation_map;
};
// Helper init functions
inline typename sga_statics<>::selection_map_t init_selection_map()
{
    typename sga_statics<>::selection_map_t retval;
    using value_type = typename sga_statics<>::selection_map_t::value_type;
    retval.insert(value_type("tournament", sga_statics<>::selection::TOURNAMENT));
    retval.insert(value_type("truncated", sga_statics<>::selection::TRUNCATED));
    return retval;
}
inline typename sga_statics<>::crossover_map_t init_crossover_map()
{
    typename sga_statics<>::crossover_map_t retval;
    using value_type = typename sga_statics<>::crossover_map_t::value_type;
    retval.insert(value_type("exponential", sga_statics<>::crossover::EXPONENTIAL));
    retval.insert(value_type("binomial", sga_statics<>::crossover::BINOMIAL));
    retval.insert(value_type("sbx", sga_statics<>::crossover::SBX));
    retval.insert(value_type("single", sga_statics<>::crossover::SINGLE));
    return retval;
}
inline typename sga_statics<>::mutation_map_t init_mutation_map()
{
    typename sga_statics<>::mutation_map_t retval;
    using value_type = typename sga_statics<>::mutation_map_t::value_type;
    retval.insert(value_type("gaussian", sga_statics<>::mutation::GAUSSIAN));
    retval.insert(value_type("uniform", sga_statics<>::mutation::UNIFORM));
    retval.insert(value_type("polynomial", sga_statics<>::mutation::POLYNOMIAL));
    return retval;
}
// We now init the various members
template <typename T>
const typename sga_statics<T>::selection_map_t sga_statics<T>::m_selection_map = init_selection_map();
template <typename T>
const typename sga_statics<T>::crossover_map_t sga_statics<T>::m_crossover_map = init_crossover_map();
template <typename T>
const typename sga_statics<T>::mutation_map_t sga_statics<T>::m_mutation_map = init_mutation_map();
} // end namespace detail

/// A Simple Genetic Algorithm
/**
 * \image html sga.jpg "The DNA Helix" width=3cm
 *
 * Approximately in the same decade as Evolutionary Strategies (see pagmo::sea) were studied, a different group
 * led by John Holland, and later by his student David Goldberg, introduced and studied an algorithmic framework called
 * "genetic algorithms" that were, essentially, leveraging on the same idea but introducing also crossover as a genetic
 * operator. This led to a few decades of confusion and discussions on what was an evolutionary startegy and what a
 * genetic algorithm and on whether the crossover was a useful operator or mutation only algorithms were to be
 * preferred.
 *
 * In pagmo we provide a rather classical implementation of a genetic algorithm, letting the user choose between
 * some selected crossover types, selection schemes,, mutation types and reinsertion scheme.
 *
 * The various blocks of pagmo genetic algorithm are listed below:
 *
 * Selection: two selection methods are provided: "tournament" and "truncated". Tournament selection works by
 * selecting each offspring as the one having the minimal fitness in a random group of \p param_s. The truncated
 * selection, instead, works selecting the best \p param_s chromosomes in the entire population over and over.
 * We have deliberately not implemented the popular roulette wheel selection as we are of the opinion that such
 * a system does not generalize much being highly sensitive to the fitness scaling.
 *
 * Crossover: four different crossover schemes are provided: "single", "exponential", "binomial", "sbx". The
 * single point crossover, called "single", works selecting a random point in the parent chromosome and inserting the
 * partner chromosome thereafter. The exponential crossover is taken from the algorithm differential evolution,
 * implemented, in pagmo, as pagmo::de. It essentially selects a random point in the parent chromosome and inserts,
 * in each successive gene, the partner values with probability \p cr up to when it stops. The binomial crossover
 * inserts each gene from the partner with probability \p cr. The simulated binary crossover (called "sbx"), is taken
 * from the NSGAII algorithm, implemented in pagmo as pagmo::nsga2, and makes use of an additional parameter called
 * distribution index \p eta_c.
 *
 * Reinsertion: the only reinsertion strategy provided is what we called simple elitism. After one generation
 * the best \p elitism parents are kept in the new population while the worst \p elitism offsprings are killed.
 *
 * **NOTE** This algorithm will work only for box bounded problems.
 *
 * **NOTE** Specifying the parameter \p int_dim a part of the decision vector (at the end) will be treated as integers
 * This means that all genetic operators are guaranteed to produce integer decision vectors in the specified bounds.
 */
class sga : private detail::sga_statics<>
{
public:
    /// Single entry of the log (gen, fevals, best, cur_best)
    // typedef std::tuple<unsigned, unsigned long long, double, double> log_line_type;
    /// The log
    // typedef std::vector<log_line_type> log_type;

    /// Constructor
    /**
     * Constructs a simple genetic algorithm.
     *
     * @param gen number of generations.
     * @param cr crossover probability. This parameter is inactive when the single-point crossover method "single" is
     * selected.
     * @param eta_c distribution index for "sbx" crossover. This is an inactive parameter if other types of crossovers
     * are selected.
     * @param m mutation probability.
     * @param param_m distribution index (in polynomial mutation), otherwise width of the mutation.
     * @param elitism number of parents that gets carried over to the next generation.
     * @param param_s when "truncated" selection is used this indicates the percentage of best individuals to use. when
     * "tournament" selection is used this indicates the size of the tournament.
     * @param mutation the mutation strategy. One of "gaussian", "polynomial" or "uniform".
     * @param selection the selection strategy. One of "tournament", "truncated".
     * @param crossover the crossover strategy. One of "exponential", "binomial", "single" or "sbx"
     * @param int_dim the number of element in the chromosome to be treated as integers.
     *
     * @throws std::invalid_argument if \p cr not in [0,1), \p eta_c not in [1, 100), \p m not in [0,1], \p elitism < 1
     * \p mutation not one of "gaussian", "uniform" or "polynomial", \p selection not one of "roulette" or "truncated"
     * \p crossover not one of "exponential", "binomial", "sbx" or "single", if \p param_m is not in [0,1] and
     * \p mutation is not "polynomial" or \p mutation is not in [1,100] and \p mutation is polynomial.
     */
    sga(unsigned gen = 1u, double cr = .95, double eta_c = 10., double m = 0.02, double param_m = 0.5,
        unsigned elitism = 5u, unsigned param_s = 5u, std::string mutation = "gaussian",
        std::string selection = "tournament", std::string crossover = "exponential",
        vector_double::size_type int_dim = 0u, unsigned seed = pagmo::random_device::next())
        : m_gen(gen), m_cr(cr), m_eta_c(eta_c), m_m(m), m_param_m(param_m), m_elitism(elitism), m_param_s(param_s),
          m_int_dim(int_dim), m_e(seed), m_seed(seed), m_verbosity(0u) //, m_log()
    {
        if (cr > 1. || cr < 0.) {
            pagmo_throw(std::invalid_argument, "The crossover probability must be in the [0,1] range, while a value of "
                                                   + std::to_string(cr) + " was detected");
        }
        if (eta_c < 1. || eta_c >= 100.) {
            pagmo_throw(std::invalid_argument,
                        "The distribution index for SBX crossover must be in [1, 100[, while a value of "
                            + std::to_string(eta_c) + " was detected");
        }
        if (m < 0. || m > 1.) {
            pagmo_throw(std::invalid_argument, "The mutation probability must be in the [0,1] range, while a value of "
                                                   + std::to_string(cr) + " was detected");
        }
        if (param_s == 0u) {
            pagmo_throw(std::invalid_argument, "The selection parameter must be at least 1, while a value of "
                                                   + std::to_string(param_s) + " was detected");
        }
        if (mutation != "gaussian" && mutation != "uniform" && mutation != "polynomial") {
            pagmo_throw(
                std::invalid_argument,
                R"(The mutation type must either be "gaussian" or "uniform" or "polynomial": unknown type requested: )"
                    + mutation);
        }
        if (selection != "truncated" && selection != "tournament") {
            pagmo_throw(
                std::invalid_argument,
                R"(The selection type must either be "roulette" or "truncated" or "tournament": unknown type requested: )"
                    + selection);
        }
        if (crossover != "exponential" && crossover != "binomial" && crossover != "sbx" && crossover != "single") {
            pagmo_throw(
                std::invalid_argument,
                R"(The crossover type must either be "exponential" or "binomial" or "sbx" or "single": unknown type requested: )"
                    + crossover);
        }
        // param_m represents the distribution index if polynomial mutation is selected
        if (mutation == "polynomial" && (param_m < 1. || param_m > 100.)) {
            pagmo_throw(
                std::invalid_argument,
                "Polynomial mutation was selected, the mutation parameter must be in [1, 100], while a value of "
                    + std::to_string(param_m) + " was detected");
        }

        // otherwise param_m represents the width of the mutation relative to the box bounds
        if (mutation != "polynomial" && (param_m < 0 || param_m > 1.)) {
            pagmo_throw(std::invalid_argument, "The mutation parameter must be in [0,1], while a value of "
                                                   + std::to_string(param_m) + " was detected");
        }
        // We can now init the data members representing the various choices made using std::string
        m_selection = m_selection_map.left.at(selection);
        m_crossover = m_crossover_map.left.at(crossover);
        m_mutation = m_mutation_map.left.at(mutation);
    }

    /// Algorithm evolve method (juice implementation of the algorithm)
    /**
     * Evolves the population for a maximum number of generations
     *
     * @param pop population to be evolved
     * @return evolved population
     * @throws std::invalid_argument if the problem is multi-objective or constrained
     * @throws std::invalid_argument if the population size is smaller than 2
     */
    population evolve(population pop) const
    {
        const auto &prob = pop.get_problem();
        auto dim = prob.get_nx();
        const auto bounds = prob.get_bounds();
        const auto &lb = bounds.first;
        const auto &ub = bounds.second;
        auto NP = pop.size();
        auto fevals0 = prob.get_fevals(); // fevals already made
        auto count = 1u;                  // regulates the screen output
        // PREAMBLE-------------------------------------------------------------------------------------------------
        // Check whether the problem/population are suitable for bee_colony
        if (prob.get_nc() != 0u) {
            pagmo_throw(std::invalid_argument, "Constraints detected in " + prob.get_name() + " instance. " + get_name()
                                                   + " cannot deal with them");
        }
        if (prob.get_nf() != 1u) {
            pagmo_throw(std::invalid_argument, "Multiple objectives detected in " + prob.get_name() + " instance. "
                                                   + get_name() + " cannot deal with them");
        }
        if (NP < 2u) {
            pagmo_throw(std::invalid_argument, prob.get_name() + " needs at least 2 individuals in the population, "
                                                   + std::to_string(NP) + " detected");
        }
        if (m_elitism > pop.size()) {
            pagmo_throw(std::invalid_argument,
                        "The elitism must be smaller than the population size, while a value of: "
                            + std::to_string(m_elitism) + " was detected in a population of size: "
                            + std::to_string(pop.size()));
        }
        if (m_param_s > pop.size()) {
            pagmo_throw(std::invalid_argument,
                        "The parameter for selection must be smaller than the population size, while a value of: "
                            + std::to_string(m_param_s) + " was detected in a population of size: "
                            + std::to_string(pop.size()));
        }
        if (m_crossover == crossover::SBX && (pop.size() % 2 != 0u)) {
            pagmo_throw(std::invalid_argument,
                        "Population size must be even if sbx crossover is selected. Detected pop size is: "
                            + std::to_string(pop.size()));
        }
        // Get out if there is nothing to do.
        if (m_gen == 0u) {
            return pop;
        }
        // ---------------------------------------------------------------------------------------------------------

        // No throws, all valid: we clear the logs
        // m_log.clear();

        for (decltype(m_gen) i = 1u; i <= m_gen; ++i) {
            // 0 - if the problem is stochastic we change seed and re-evaluate the entire population
            if (prob.is_stochastic()) {
                pop.get_problem().set_seed(std::uniform_int_distribution<unsigned int>()(m_e));
                // re-evaluate the whole population w.r.t. the new seed
                for (decltype(pop.size()) j = 0u; j < pop.size(); ++j) {
                    pop.set_xf(j, pop.get_x()[j], prob.fitness(pop.get_x()[j]));
                }
            }
            auto XNEW = pop.get_x();
            auto FNEW = pop.get_f();
            // 1 - Selection.
            auto selected_idx = perform_selection(FNEW);
            for (decltype(XNEW.size()) i = 0u; i < XNEW.size(); ++i) {
                XNEW[i] = pop.get_x()[selected_idx[i]];
            }
            // 2 - Crossover
            perform_crossover(XNEW, prob.get_bounds());
            // 3 - Mutation
            perform_mutation(XNEW, prob.get_bounds());
            // 4 - Evaluate the new population
            for (decltype(XNEW.size()) i = 0u; i < XNEW.size(); ++i) {
                FNEW[i] = prob.fitness(XNEW[i]);
            }
            // 5 - Reinsertion
            // We sort the original population
            std::vector<vector_double::size_type> best_parents(pop.get_f().size());
            std::iota(best_parents.begin(), best_parents.end(), vector_double::size_type(0u));
            std::sort(best_parents.begin(), best_parents.end(),
                      [pop](vector_double::size_type a, vector_double::size_type b) {
                          return detail::less_than_f(pop.get_f()[a][0], pop.get_f()[b][0]);
                      });
            // We sort the new population
            std::vector<vector_double::size_type> best_offsprings(FNEW.size());
            std::iota(best_offsprings.begin(), best_offsprings.end(), vector_double::size_type(0u));
            std::sort(best_offsprings.begin(), best_offsprings.end(),
                      [FNEW](vector_double::size_type a, vector_double::size_type b) {
                          return detail::less_than_f(FNEW[a][0], FNEW[b][0]);
                      });
            // We re-insert m_elitism best parents and the remaining best children
            population pop_copy(pop);
            for (decltype(m_elitism) i = 0u; i < m_elitism; ++i) {
                pop.set_xf(i, pop_copy.get_x()[best_parents[i]], pop_copy.get_f()[best_parents[i]]);
            }
            for (decltype(pop.size()) i = m_elitism; i < pop.size(); ++i) {
                pop.set_xf(i, XNEW[best_offsprings[i]], FNEW[best_offsprings[i]]);
            }
        }
        return pop;
    }

    /// Sets the seed
    /**
     * @param seed the seed controlling the algorithm stochastic behaviour
     */
    void set_seed(unsigned seed)
    {
        m_e.seed(seed);
        m_seed = seed;
    }
    /// Gets the seed
    /**
    * @return the seed controlling the algorithm stochastic behaviour
    */
    unsigned get_seed() const
    {
        return m_seed;
    }
    /// Sets the algorithm verbosity
    /**
    * Sets the verbosity level of the screen output and of the
    * log returned by get_log(). \p level can be:
    * - 0: no verbosity
    * - >0: will print and log one line each \p level generations.
    *
    * Example (verbosity 100):
    * @code{.unparsed}
    *     Gen:        Fevals:          Best: Current Best:
    *        1             40         261363         261363
    *      101           4040        112.237        267.969
    *      201           8040        20.8885        265.122
    *      301          12040        20.6076        20.6076
    *      401          16040         18.252        140.079
    * @endcode
    * Gen is the generation number, Fevals the number of function evaluation used, , Best is the best fitness found,
    * Current best is the best fitness currently in the population.
    *
    * @param level verbosity level
    */
    void set_verbosity(unsigned level)
    {
        m_verbosity = level;
    }
    /// Gets the verbosity level
    /**
    * @return the verbosity level
    */
    unsigned get_verbosity() const
    {
        return m_verbosity;
    }
    /// Algorithm name
    /**
    * @return a string containing the algorithm name
    */
    std::string get_name() const
    {
        return "Genetic Algorithm";
    }
    /// Extra informations
    /**
    * @return a string containing extra informations on the algorithm
    */
    std::string get_extra_info() const
    {
        std::ostringstream ss;
        stream(ss, "\tNumber of generations: ", m_gen);
        stream(ss, "\n\tElitism: ", m_elitism);
        stream(ss, "\n\tCrossover:");
        stream(ss, "\n\t\tType: " + m_crossover_map.right.at(m_crossover));
        stream(ss, "\n\t\tProbability: ", m_cr);
        if (m_crossover == crossover::SBX) stream(ss, "\n\t\tDistribution index: ", m_eta_c);
        stream(ss, "\n\tMutation:");
        stream(ss, "\n\t\tType: ", m_mutation_map.right.at(m_mutation));
        stream(ss, "\n\t\tProbability: ", m_m);
        if (m_mutation != mutation::POLYNOMIAL) {
            stream(ss, "\n\t\tWidth: ", m_param_m);
        } else {
            stream(ss, "\n\t\tDistribution index: ", m_param_m);
        }
        stream(ss, "\n\tSelection:");
        stream(ss, "\n\t\tType: ", m_selection_map.right.at(m_selection));
        if (m_selection == selection::TRUNCATED) stream(ss, "\n\t\tTruncation size: ", m_param_s);
        if (m_selection == selection::TOURNAMENT) stream(ss, "\n\t\tTournament size: ", m_param_s);
        stream(ss, "\n\tSize of the integer part: ", m_int_dim);
        stream(ss, "\n\tSeed: ", m_seed);
        stream(ss, "\n\tVerbosity: ", m_verbosity);
        return ss.str();
    }

    /// Get log
    /**
    * A log containing relevant quantities monitoring the last call to evolve. Each element of the returned
    * <tt>std::vector</tt> is a bee_colony::log_line_type containing: Gen, Fevals, Current best, Best as
    * described in bee_colony::set_verbosity().
    *
    * @return an <tt> std::vector</tt> of bee_colony::log_line_type containing the logged values Gen, Fevals, Current
    * best, Best
    */
    // const log_type &get_log() const
    //{
    //    return m_log;
    //}
    /// Object serialization
    /**
    * This method will save/load \p this into the archive \p ar.
    *
    * @param ar target archive.
    *
    * @throws unspecified any exception thrown by the serialization of the UDP and of primitive types.
    */
    template <typename Archive>
    void serialize(Archive &ar)
    {
        ar(m_gen, m_cr, m_eta_c, m_m, m_param_m, m_elitism, m_param_s, m_mutation, m_selection, m_crossover, m_int_dim,
           m_e, m_seed, m_verbosity);
    }

public:
    std::vector<vector_double::size_type> perform_selection(const std::vector<vector_double> &F) const
    {
        assert(m_param_s <= F.size());
        std::vector<vector_double::size_type> retval(F.size());
        std::vector<vector_double::size_type> best_idxs(F.size());
        std::iota(best_idxs.begin(), best_idxs.end(), vector_double::size_type(0u));
        switch (m_selection) {
            case (selection::TRUNCATED): {
                std::sort(best_idxs.begin(), best_idxs.end(),
                          [&F](vector_double::size_type a, vector_double::size_type b) {
                              return detail::less_than_f(F[a][0], F[b][0]);
                          });
                for (decltype(retval.size()) i = 0u; i < retval.size(); ++i) {
                    retval[i] = best_idxs[i % m_param_s];
                }
                break;
            }
            case (selection::TOURNAMENT): {
                // We make one tournament for each of the offspring to be generated
                for (decltype(retval.size()) j = 0u; j < retval.size(); ++j) {
                    // Fisher Yates algo http://en.wikipedia.org/wiki/Fisher%E2%80%93Yates_shuffle
                    // to select m_param_s individial at random
                    for (decltype(m_param_s) i = 0u; i < m_param_s; ++i) {
                        auto index = std::uniform_int_distribution<std::vector<vector_double::size_type>::size_type>(
                            i, best_idxs.size() - 1)(m_e);
                        std::swap(best_idxs[index], best_idxs[i]);
                    }
                    // Find the index of the individual with minimum fitness among the randomly selected ones
                    double winner = best_idxs[0];
                    for (decltype(m_param_s) i = 1u; i < m_param_s; ++i) {
                        if (F[best_idxs[i]] < F[winner]) {
                            winner = best_idxs[i];
                        }
                    }
                    retval[j] = winner;
                }
                break;
            }
        }
        return retval;
    }
    void perform_crossover(std::vector<vector_double> &X, const std::pair<vector_double, vector_double> &bounds) const
    {
        auto dim = X[0].size();
        assert(X.size() > 1u);
        assert(std::all_of(X.begin(), X.end(), [](const vector_double &item) { return item.size() == dim; }));
        std::vector<vector_double::size_type> all_idx(X.size()); // stores indexes to then select one at random
        std::iota(all_idx.begin(), all_idx.end(), vector_double::size_type(0u));
        std::uniform_real_distribution<> drng(0., 1.);
        // We need different loops if the crossover type is "sbx"" as this method creates two offsprings per
        // selected couple.
        if (m_crossover == crossover::SBX) {
            assert(X.size() % 2u == 0u);
            std::shuffle(X.begin(), X.end(), m_e);
            for (decltype(X.size()) i = 0u; i < X.size(); i += 2) {
                auto children = sbx_crossover_impl(X[i], X[i + 1], bounds);
                X[i] = children.first;
                X[i + 1] = children.second;
            }
        } else {
            auto XCOPY = X;
            // Start of main loop through the X
            for (decltype(X.size()) i = 0u; i < X.size(); ++i) {
                // 1 - we select a mating partner
                std::swap(all_idx[0], all_idx[i]);
                auto partner_idx = std::uniform_int_distribution<std::vector<vector_double::size_type>::size_type>(
                    1, all_idx.size() - 1)(m_e);
                // 2 - We rename these chromosomes for code clarity
                auto &child = X[i];
                const auto &parent2 = XCOPY[all_idx[partner_idx]];
                // 3 - We perform crossover according to the selected method
                switch (m_crossover) {
                    case (crossover::EXPONENTIAL): {
                        auto n = std::uniform_int_distribution<std::vector<vector_double::size_type>::size_type>(
                            0, dim - 1u)(m_e);
                        decltype(dim) L = 0u;
                        do {
                            child[n] = parent2[n];
                            n = (n + 1u) % dim;
                            L++;
                        } while ((drng(m_e) < m_cr) && (L < dim));
                        break;
                    }
                    case (crossover::BINOMIAL): {
                        auto n = std::uniform_int_distribution<std::vector<vector_double::size_type>::size_type>(
                            0, dim - 1u)(m_e);
                        for (decltype(dim) L = 0u; L < dim; ++L) {    /* performs D binomial trials */
                            if ((drng(m_e) < m_cr) || L + 1 == dim) { /* changes at least one parameter */
                                child[n] = parent2[n];
                            }
                            n = (n + 1) % dim;
                        }
                        break;
                    }
                    case (crossover::SINGLE): {
                        auto n = std::uniform_int_distribution<std::vector<vector_double::size_type>::size_type>(
                            0, dim)(m_e);
                        std::copy(parent2.data() + n, parent2.data() + dim, child.data() + n);
                        break;
                    }
                    // LCOV_EXCL_START
                    default: {
                        pagmo_throw(std::logic_error, "The code should never reach here");
                        break;
                    }
                        // LCOV_EXCL_STOP
                }
            }
        }
    }
    void perform_mutation(std::vector<vector_double> &X, const std::pair<vector_double, vector_double> &bounds) const
    {
        // Some dimensions
        auto dim = X[0].size();
        auto dimi = m_int_dim;
        auto dimc = dim - dimi;
        // Problem bounds
        const auto &lb = bounds.first;
        const auto &ub = bounds.second;
        assert(X.size() > 1u);
        assert(std::all_of(X.begin(), X.end(), [](const vector_double &item) { return item.size() == dim; }));
        assert(dimc >= 0u);
        // Random distributions
        std::uniform_real_distribution<> drng(0., 1.); // to generate a number in [0, 1)
        // Start of main loop through the population
        for (decltype(X.size()) i = 0u; i < X.size(); ++i) {
            switch (m_mutation) {
                case (mutation::UNIFORM): {
                    // Start of main loop through the chromosome (continuous part)
                    for (decltype(dim) j = 0u; j < dimc; ++j) {
                        X[i][j] = lb[j] + drng(m_e) * (ub[j] - lb[j]);
                        break;
                    }
                }
            }
        }
    }
    std::pair<vector_double, vector_double>
    sbx_crossover_impl(const vector_double &parent1, const vector_double &parent2,
                       const std::pair<vector_double, vector_double> &bounds) const
    {
        // Decision vector dimensions
        auto D = parent1.size();
        auto Di = m_int_dim;
        auto Dc = D - Di;
        // Problem bounds
        const auto &lb = bounds.first;
        const auto &ub = bounds.second;
        // declarations
        double y1, y2, yl, yu, rand01, beta, alpha, betaq, c1, c2;
        vector_double::size_type site1, site2;
        // Initialize the child decision vectors
        auto child1 = parent1;
        auto child2 = parent2;
        // Random distributions
        std::uniform_real_distribution<> drng(0., 1.); // to generate a number in [0, 1)

        // This implements a Simulated Binary Crossover SBX and applies it to the non integer part of the decision
        // vector
        if (drng(m_e) <= m_cr) {
            for (decltype(Dc) i = 0u; i < Dc; i++) {
                if ((drng(m_e) <= 0.5) && (std::abs(parent1[i] - parent2[i])) > 1e-14 && lb[i] != ub[i]) {
                    if (parent1[i] < parent2[i]) {
                        y1 = parent1[i];
                        y2 = parent2[i];
                    } else {
                        y1 = parent2[i];
                        y2 = parent1[i];
                    }
                    yl = lb[i];
                    yu = ub[i];
                    rand01 = drng(m_e);
                    beta = 1. + (2. * (y1 - yl) / (y2 - y1));
                    alpha = 2. - std::pow(beta, -(m_eta_c + 1.));
                    if (rand01 <= (1. / alpha)) {
                        betaq = std::pow((rand01 * alpha), (1. / (m_eta_c + 1.)));
                    } else {
                        betaq = std::pow((1. / (2. - rand01 * alpha)), (1. / (m_eta_c + 1.)));
                    }
                    c1 = 0.5 * ((y1 + y2) - betaq * (y2 - y1));
                    beta = 1. + (2. * (yu - y2) / (y2 - y1));
                    alpha = 2. - std::pow(beta, -(m_eta_c + 1.));
                    if (rand01 <= (1. / alpha)) {
                        betaq = std::pow((rand01 * alpha), (1. / (m_eta_c + 1.)));
                    } else {
                        betaq = std::pow((1. / (2. - rand01 * alpha)), (1. / (m_eta_c + 1.)));
                    }
                    c2 = 0.5 * ((y1 + y2) + betaq * (y2 - y1));
                    if (c1 < lb[i]) c1 = lb[i];
                    if (c2 < lb[i]) c2 = lb[i];
                    if (c1 > ub[i]) c1 = ub[i];
                    if (c2 > ub[i]) c2 = ub[i];
                    if (drng(m_e) <= .5) {
                        child1[i] = c1;
                        child2[i] = c2;
                    } else {
                        child1[i] = c2;
                        child2[i] = c1;
                    }
                }
            }
        }
        // This implements two-point binary crossover and applies it to the integer part of the chromosome
        for (decltype(Dc) i = Dc; i < D; ++i) {
            // in this loop we are sure Di is at least 1
            std::uniform_int_distribution<vector_double::size_type> ra_num(0, Di - 1u);
            if (drng(m_e) <= m_cr) {
                site1 = ra_num(m_e);
                site2 = ra_num(m_e);
                if (site1 > site2) {
                    std::swap(site1, site2);
                }
                for (decltype(site1) j = 0u; j < site1; ++j) {
                    child1[j] = parent1[j];
                    child2[j] = parent2[j];
                }
                for (decltype(site2) j = site1; j < site2; ++j) {
                    child1[j] = parent2[j];
                    child2[j] = parent1[j];
                }
                for (decltype(Di) j = site2; j < Di; ++j) {
                    child1[j] = parent1[j];
                    child2[j] = parent2[j];
                }
            } else {
                child1[i] = parent1[i];
                child2[i] = parent2[i];
            }
        }
        return std::pair<vector_double, vector_double>(std::move(child1), std::move(child2));
    }
    unsigned m_gen;
    double m_cr;
    double m_eta_c;
    double m_m;
    double m_param_m;
    unsigned m_elitism;
    unsigned m_param_s;
    mutation m_mutation;
    selection m_selection;
    crossover m_crossover;
    vector_double::size_type m_int_dim;
    mutable detail::random_engine_type m_e;
    unsigned int m_seed;
    unsigned int m_verbosity;
    // mutable log_type m_log;
};

} // namespace pagmo

PAGMO_REGISTER_ALGORITHM(pagmo::sga)

#endif
