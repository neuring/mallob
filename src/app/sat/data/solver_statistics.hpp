
#pragma once

#include <string>

#include "clause_histogram.hpp"

struct SolverStatistics {

	unsigned long propagations = 0;
	unsigned long decisions = 0;
	unsigned long conflicts = 0;
	unsigned long restarts = 0;
	double memPeak = 0;
	unsigned long imported = 0;
	unsigned long discarded = 0;
    unsigned long conflicts_on_imported_clauses = 0; 
    unsigned long propagations_on_imported_clauses = 0; 
	unsigned long imported_clauses = 0;
	unsigned long imported_clauses_bc_selection_heuristic = 0;
	
	// clause export
	ClauseHistogram* histProduced;
	unsigned long producedClauses = 0;
	unsigned long producedClausesFiltered = 0;
	unsigned long producedClausesAdmitted = 0;
	unsigned long producedClausesDropped = 0;

	// clause import
	ClauseHistogram* histDigested;
	unsigned long receivedClauses = 0;
	unsigned long receivedClausesFiltered = 0;
	unsigned long receivedClausesDigested = 0;
	unsigned long receivedClausesDropped = 0;

	std::string getReport() const {
		return "pps:" + std::to_string(propagations)
			+ " ppsi:" + std::to_string(propagations_on_imported_clauses)
			+ " dcs:" + std::to_string(decisions)
			+ " cfs:" + std::to_string(conflicts)
			+ " cfsi:" + std::to_string(conflicts_on_imported_clauses)
			+ " rst:" + std::to_string(restarts)
			+ " imp:" + std::to_string(imported_clauses)
			+ " impsh:" + std::to_string(imported_clauses_bc_selection_heuristic)
			+ " prod:" + std::to_string(producedClauses)
			+ " (flt:" + std::to_string(producedClausesFiltered)
			+ " adm:" + std::to_string(producedClausesAdmitted)
			+ " drp:" + std::to_string(producedClausesDropped)
			+ ") recv:" + std::to_string(receivedClauses)
			+ " (flt:" + std::to_string(receivedClausesFiltered)
			+ " digd:" + std::to_string(receivedClausesDigested)
			+ " drp:" + std::to_string(receivedClausesDropped)
			+ ") + intim:" + std::to_string(imported) + "/" + std::to_string(imported+discarded);
	}

	void aggregate(const SolverStatistics& other) {
		propagations += other.propagations;
		decisions += other.decisions;
		conflicts += other.conflicts;
		restarts += other.restarts;
		memPeak += other.memPeak;
		conflicts_on_imported_clauses += other.conflicts_on_imported_clauses;
		producedClauses += other.producedClauses;
		producedClausesAdmitted += other.producedClausesAdmitted;
		producedClausesFiltered += other.producedClausesFiltered;
		producedClausesDropped += other.producedClausesDropped;
		receivedClauses += other.receivedClauses;
		receivedClausesFiltered += other.receivedClausesFiltered;
		receivedClausesDigested += other.receivedClausesDigested;
		receivedClausesDropped += other.receivedClausesDropped;
		imported_clauses += other.imported_clauses;
		imported_clauses_bc_selection_heuristic += other.imported_clauses_bc_selection_heuristic;
	}
};
