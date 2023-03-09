/*
 * Cadical.cpp
 *
 *  Created on: Jun 26, 2020
 *      Author: schick
 */

#include <ctype.h>
#include <stdarg.h>
#include <chrono>
#include <string>
#include <iostream>

#include "cadical.hpp"
#include "util/distribution.hpp"

Cadical::Cadical(const SolverSetup& setup)
	: PortfolioSolverInterface(setup),
	  solver(new CaDiCaL::Solver), terminator(*setup.logger), 
	  learner(_setup), learnSource(_setup, [this]() {
		  Mallob::Clause c;
		  fetchLearnedClause(c, AdaptiveClauseDatabase::ANY);
		  return c;
	  }) {
	
	for (auto& [key, value] : setup.solver_flags) {
		if (key[0] == 'c') {
			std::string cadical_key = key.substr(2);
			solver->set(cadical_key.c_str(), std::stoi(value));
		}
	}
	
	solver->connect_terminator(&terminator);
	solver->connect_learn_source(&learnSource);
}

void Cadical::addLiteral(int lit) {
	solver->add(lit);
}

void Cadical::diversify(int seed) {

	if (seedSet) return;

	LOGGER(_logger, V3_VERB, "Diversifying %i\n", getDiversificationIndex());
	bool okay = solver->set("seed", seed);
	assert(okay);
	switch (getDiversificationIndex() % getNumOriginalDiversifications()) {
	// Greedy 10-portfolio according to tests of the above configurations on SAT2020 instances
	case 0: okay = solver->set("phase", 0); break;
	case 1: okay = solver->configure("sat"); break;
	case 2: okay = solver->set("elim", 0); break;
	case 3: okay = solver->configure("unsat"); break;
	case 4: okay = solver->set("condition", 1); break;
	case 5: okay = solver->set("walk", 0); break;
	case 6: okay = solver->set("restartint", 100); break;
	case 7: okay = solver->set("cover", 1); break;
	case 8: okay = solver->set("shuffle", 1) && solver->set("shufflerandom", 1); break;
	case 9: okay = solver->set("inprocessing", 0); break;
	}
	assert(okay);

	// Randomize ("jitter") certain options around their default value
    if (getDiversificationIndex() >= getNumOriginalDiversifications()) {
        std::mt19937 rng(seed);
        Distribution distribution(rng);

        // Randomize restart frequency
        double meanRestarts = solver->get("restartint");
        double maxRestarts = std::min(2e9, 20*meanRestarts);
        distribution.configure(Distribution::NORMAL, std::vector<double>{
            /*mean=*/meanRestarts, /*stddev=*/10, /*min=*/1, /*max=*/maxRestarts
        });
        int restartFrequency = (int) std::round(distribution.sample());
        okay = solver->set("restartint", restartFrequency); assert(okay);

        // Randomize score decay
        double meanDecay = solver->get("scorefactor");
        distribution.configure(Distribution::NORMAL, std::vector<double>{
            /*mean=*/meanDecay, /*stddev=*/3, /*min=*/500, /*max=*/1000
        });
        int decay = (int) std::round(distribution.sample());
        okay = solver->set("scorefactor", decay); assert(okay);
        
        LOGGER(_logger, V3_VERB, "Sampled restartint=%i decay=%i\n", restartFrequency, decay);
    }

	seedSet = true;
	setClauseSharing(getNumOriginalDiversifications());
}

int Cadical::getNumOriginalDiversifications() {
	return 10;
}

void Cadical::setPhase(const int var, const bool phase) {
	solver->phase(phase ? var : -var);
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult Cadical::solve(size_t numAssumptions, const int* assumptions) {

	// add the learned clauses
	learnMutex.lock();
	for (auto clauseToAdd : learnedClauses) {
		for (auto litToAdd : clauseToAdd) {
			addLiteral(litToAdd);
		}
		addLiteral(0);
	}
	learnedClauses.clear();
	learnMutex.unlock();

	// set the assumptions
	this->assumptions.clear();
	for (size_t i = 0; i < numAssumptions; i++) {
		int lit = assumptions[i];
		solver->assume(lit);
		this->assumptions.push_back(lit);
	}

	// start solving
	int res = solver->solve();
	switch (res) {
	case 0:
		return UNKNOWN;
	case 10:
		return SAT;
	case 20:
		return UNSAT;
	default:
		return UNKNOWN;
	}
}

void Cadical::setSolverInterrupt() {
	terminator.setInterrupt();
}

void Cadical::unsetSolverInterrupt() {
	terminator.unsetInterrupt();
}

void Cadical::setSolverSuspend() {
    terminator.setSuspend();
}

void Cadical::unsetSolverSuspend() {
    terminator.unsetSuspend();
}

std::vector<int> Cadical::getSolution() {
	std::vector<int> result = {0};

	for (int i = 1; i <= getVariablesCount(); i++)
		result.push_back(solver->val(i));

	return result;
}

std::set<int> Cadical::getFailedAssumptions() {
	std::set<int> result;
	for (auto assumption : assumptions)
		if (solver->failed(assumption))
			result.insert(assumption);

	return result;
}

void Cadical::setLearnedClauseCallback(const LearnedClauseCallback& callback) {
	learner.setCallback(callback);
	solver->connect_learner(&learner);
}

int Cadical::getVariablesCount() {
	return solver->vars();
}

int Cadical::getSplittingVariable() {
	return solver->lookahead();
}

void Cadical::writeStatistics(SolverStatistics& stats) {
	CaDiCaL::Solver::Statistics s = solver->get_stats();
	stats.conflicts = s.conflicts;
	stats.decisions = s.decisions;
	stats.propagations = s.propagations;
	stats.restarts = s.restarts;
	stats.conflicts_on_imported_clauses = s.conflicts_on_imported_clauses;
	stats.propagations_on_imported_clauses = s.propagations_on_imported_clauses;
	stats.imported_clauses = s.imported_clauses;
}

Cadical::~Cadical() {
	solver.release();
}
