// Copyright (c) 2008, Jacob Burnim (jburnim@cs.berkeley.edu)
//
// This file is part of CREST, which is distributed under the revised
// BSD license.  A copy of this license can be found in the file LICENSE.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See LICENSE
// for details.

#include <iostream>
#include <assert.h>
#include <limits>
#include <queue>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <utility>

// #include <yices_c.h>   // version1
#include "yices.h"        // version2

#include "base/yices_solver.h"

using namespace std;
using std::make_pair;
using std::numeric_limits;
using std::queue;
using std::set;

static void print_term(term_t term) {
  char *s; 

  s = yices_term_to_string(term, 80, 20, 0);
  if (s == NULL) {
    // An error occurred
    s = yices_error_string();
    fprintf(stderr, "Error in print_term: %s\n", s);
    yices_free_string(s);
    exit(1);
  }
  // print the string then free it
  printf("%s\n", s);
  yices_free_string(s);
}


namespace crest {

typedef vector<const SymbolicPred*>::const_iterator PredIt;


term_t makeYicesNum(value_t val) {
  // Send the constant term to Yices as a string, to correctly handle constant terms outside
  // the range of integers.
  //
  // NOTE: This is not correct for unsigned long long values that are larger than the max
  // long long int value.
  char buff[32];
  snprintf(buff, 32, "%lf", val);
  return yices_parse_float(buff);
}


bool YicesSolver::IncrementalSolve(const vector<value_t>& old_soln,
				   const map<var_t,type_t>& vars,
				   const vector<const SymbolicPred*>& constraints,
				   map<var_t,value_t>* soln) {

  set<var_t> tmp;
  typedef set<var_t>::const_iterator VarIt;

  // Build a graph on the variables, indicating a dependence when two
  // variables co-occur in a symbolic predicate.
  vector< set<var_t> > depends(vars.size());
  for (PredIt i = constraints.begin(); i != constraints.end(); ++i) {
    tmp.clear();
    (*i)->AppendVars(&tmp);
    for (VarIt j = tmp.begin(); j != tmp.end(); ++j) {
      depends[*j].insert(tmp.begin(), tmp.end());
    }
  }

  // Initialize the set of dependent variables to those in the constraints.
  // (Assumption: Last element of constraints is the only new constraint.)
  // Also, initialize the queue for the BFS.
  map<var_t,type_t> dependent_vars;
  queue<var_t> Q;
  tmp.clear();
  constraints.back()->AppendVars(&tmp);
  for (VarIt j = tmp.begin(); j != tmp.end(); ++j) {
    dependent_vars.insert(*vars.find(*j));
    Q.push(*j);
  }

  // Run the BFS.
  while (!Q.empty()) {
    var_t i = Q.front();
    Q.pop();
    for (VarIt j = depends[i].begin(); j != depends[i].end(); ++j) {
      if (dependent_vars.find(*j) == dependent_vars.end()) {
	Q.push(*j);
	dependent_vars.insert(*vars.find(*j));
      }
    }
  }

  // Generate the list of dependent constraints.
  vector<const SymbolicPred*> dependent_constraints;
  for (PredIt i = constraints.begin(); i != constraints.end(); ++i) {
    if ((*i)->DependsOn(dependent_vars))
      dependent_constraints.push_back(*i);
  }

  soln->clear();
  if (Solve(dependent_vars, dependent_constraints, soln)) {
    // Merge in the constrained variables.
    for (PredIt i = constraints.begin(); i != constraints.end(); ++i) {
      (*i)->AppendVars(&tmp);
    }
    for (set<var_t>::const_iterator i = tmp.begin(); i != tmp.end(); ++i) {
      if (soln->find(*i) == soln->end()) {
	soln->insert(make_pair(*i, old_soln[*i]));
      }
    }
    return true;
  }

  return false;
}


bool YicesSolver::Solve(const map<var_t,type_t>& vars,
			const vector<const SymbolicPred*>& constraints,
			map<var_t,value_t>* soln) {

  yices_init();
  typedef map<var_t,type_t>::const_iterator VarIt;

  // Type limits.
  vector<term_t> min_expr(types::DOUBLE+1);
  vector<term_t> max_expr(types::DOUBLE+1);
  for (int i = types::U_CHAR; i <= types::DOUBLE; i++) {
    if (i == types::FLOAT || i == types::DOUBLE) {
      min_expr[i] = yices_parse_float(const_cast<char*>(kMinValueStr[i]));
      max_expr[i] = yices_parse_float(const_cast<char*>(kMaxValueStr[i]));
    }
    else {
      if (i%2) {
        min_expr[i] = yices_int64(kMinValue[i]);
        max_expr[i] = yices_int64(kMaxValue[i]);
      }
      else {
        min_expr[i] = yices_int64(kMinValue[i]);
        max_expr[i] = yices_int64(kMaxValue[i]);
      }
    }
    assert(min_expr[i]);
    assert(max_expr[i]);
  }
  
  // Variable declarations.
  term_t f;
  vector<term_t> terms;
  map<var_t,term_t> x_decl;
  for (VarIt i = vars.begin(); i != vars.end(); ++i) {
    char buff[32];
    snprintf(buff, sizeof(buff), "x%d", i->first);
    // fprintf(stderr, "yices_mk_var_decl(ctx, buff, int_ty)\n");
    x_decl[i->first] = yices_new_uninterpreted_term(yices_real_type());
    yices_set_term_name(x_decl[i->first], buff);
    
    // terms.push_back(x_decl[i->first]);
    assert(x_decl[i->first]);

    // fprintf(stderr, "yices_assert(ctx, yices_mk_ge(ctx, x_expr[i->first], min_expr[i->second]))\n");
    terms.push_back(yices_arith_geq_atom(x_decl[i->first], min_expr[i->second]));
    terms.push_back(yices_arith_leq_atom(x_decl[i->first], max_expr[i->second]));
  }

  { // Constraints.
    vector<term_t> term;
    for (PredIt i = constraints.begin(); i != constraints.end(); ++i) {
      const SymbolicExpr& se = (*i)->expr();
      term.clear();
      term.push_back(makeYicesNum(se.const_term()));
      for (SymbolicExpr::TermIt j = se.terms().begin(); j != se.terms().end(); ++j) {
        term.push_back(yices_mul(x_decl[j->first], makeYicesNum(j->second)));
      }
      term_t e = yices_sum(term.size(), term.data());
      term_t pred;
      switch((*i)->op()) {
      case ops::EQ:  pred = yices_arith_eq0_atom(e); break;
      case ops::NEQ: pred = yices_arith_neq0_atom(e); break;
      case ops::GT:  pred = yices_arith_gt0_atom(e); break;
      case ops::LE:  pred = yices_arith_leq0_atom(e); break;
      case ops::LT:  pred = yices_arith_lt0_atom(e); break;
      case ops::GE:  pred = yices_arith_geq0_atom(e); break;
      default:
        fprintf(stderr, "Unknown comparison operator: %d\n", (*i)->op());
        exit(1);
      }
      terms.push_back(pred);
    }
  }
  // cout << "term size: " << terms.size() << endl;
  // for (int i=0; i<terms.size(); i++) {
  //   print_term(terms[i]);
  // }

  f = yices_and(terms.size(), terms.data());
  // print_term(f);

  model_t *model;
  smt_status_t sat = yices_check_formula(f, "QF_LRA", &model, NULL);
  if (sat == STATUS_SAT) {
    soln->clear();
    for (VarIt i = vars.begin(); i != vars.end(); ++i) {
      double val;
      yices_get_double_value(model, x_decl[i->first], &val);
      soln->insert(make_pair(i->first, val));
    }
    yices_free_model(model);
  }

  yices_exit();

  return sat == STATUS_SAT;
}

}  // namespace crest
