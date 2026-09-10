//===----------------------------------------------------------------------===//
//                         DuckDB
//
// from_substrait.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <unordered_map>
#include "substrait/plan.pb.h"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/shared_ptr.hpp"

namespace duckdb {

//! How a Substrait `extract` component maps onto a DuckDB `date_part` call.
struct SubstraitExtractComponent {
	//! Sentinel for `duckdb_base`: the `indexing` option does not apply to this component.
	static constexpr int8_t NOT_INDEXED = -1;

	//! The DuckDB date_part specifier to call.
	const char *specifier;
	//! Substrait's sub-second components are relative to the next larger unit ("microseconds
	//! since the last full millisecond"), while DuckDB's are relative to the minute. Non-zero
	//! means the result needs `% modulus` to match Substrait. 0 means no fix-up.
	int64_t modulus = 0;
	//! For the components governed by the `indexing` option, the first value DuckDB counts
	//! from: date_part('dow') is 0-based, 'isodow'/'month'/'week' are 1-based.
	int8_t duckdb_base = NOT_INDEXED;
	//! UNIX_TIME is elapsed *whole* seconds. date_part('epoch') returns a DOUBLE and DuckDB's
	//! DOUBLE->BIGINT cast rounds half-to-even, so it needs an explicit floor plus a cast.
	bool epoch_seconds = false;
};

struct RootNameIterator {
	explicit RootNameIterator(const google::protobuf::RepeatedPtrField<std::string> *names) : names(names) {};
	string GetCurrentName() const {
		if (!names) {
			return "";
		}
		if (iterator >= names->size()) {
			throw InvalidInputException("Trying to access invalid root name at struct creation");
		}
		return (*names)[iterator];
	}
	void Next() {
		++iterator;
	}
	bool Unique(idx_t count) const {
		idx_t pos = iterator;
		set<string> values;
		for (idx_t i = 0; i < count; i++) {
			if (values.find((*names)[pos]) != values.end()) {
				return false;
			}
			values.insert((*names)[pos]);
			pos++;
		}
		return true;
	}
	bool Finished() const {
		if (!names) {
			return true;
		}
		return iterator >= names->size();
	}
	const google::protobuf::RepeatedPtrField<std::string> *names = nullptr;
	int iterator = 0;
};

class SubstraitToDuckDB {
public:
	SubstraitToDuckDB(shared_ptr<ClientContext> &context_p, const string &serialized, bool json = false,
	                  bool acquire_lock = false);
	//! Transforms Substrait Plan to DuckDB Relation
	shared_ptr<Relation> TransformPlan();

private:
	//! Transforms Substrait Plan Root To a DuckDB Relation
	shared_ptr<Relation> TransformRootOp(const substrait::RelRoot &sop);
	//! Transform Substrait Operations to DuckDB Relations
	shared_ptr<Relation> TransformOp(const substrait::Rel &sop,
	                                 const google::protobuf::RepeatedPtrField<std::string> *names = nullptr);
	shared_ptr<Relation> TransformJoinOp(const substrait::Rel &sop);
	shared_ptr<Relation> TransformLateralJoinOp(const substrait::Rel &sop);
	shared_ptr<Relation> TransformCrossProductOp(const substrait::Rel &sop);
	shared_ptr<Relation> TransformFetchOp(const substrait::Rel &sop,
	                                      const google::protobuf::RepeatedPtrField<std::string> *names = nullptr);
	shared_ptr<Relation> TransformFilterOp(const substrait::Rel &sop);
	shared_ptr<Relation> TransformProjectOp(const substrait::Rel &sop,
	                                        const google::protobuf::RepeatedPtrField<std::string> *names = nullptr);
	shared_ptr<Relation> TransformAggregateOp(const substrait::Rel &sop);
	shared_ptr<Relation> TransformWindowOp(const substrait::Rel &sop);
	shared_ptr<Relation> TransformReadOp(const substrait::Rel &sop);
	shared_ptr<Relation> GetValueRelationWithSingleBoolColumn();
	shared_ptr<Relation>
	GetValuesExpression(const google::protobuf::RepeatedPtrField<substrait::Expression_Nested_Struct> &expression_rows);
	shared_ptr<Relation> TransformSortOp(const substrait::Rel &sop,
	                                     const google::protobuf::RepeatedPtrField<std::string> *names = nullptr);
	shared_ptr<Relation> TransformSetOp(const substrait::Rel &sop,
	                                    const google::protobuf::RepeatedPtrField<std::string> *names = nullptr);
	shared_ptr<Relation> TransformWriteOp(const substrait::Rel &sop);
	shared_ptr<Relation> TransformReferenceOp(const substrait::Rel &sop);

	//! Transform Substrait Expressions to DuckDB Expressions
	unique_ptr<ParsedExpression> TransformExpr(const substrait::Expression &sexpr,
	                                           RootNameIterator *iterator = nullptr);
	static unique_ptr<ParsedExpression> TransformLiteralExpr(const substrait::Expression &sexpr);
	unique_ptr<ParsedExpression> TransformSelectionExpr(const substrait::Expression &sexpr);
	unique_ptr<ParsedExpression> ResolveOuterReference(
	    const substrait::Expression_FieldReference_OuterReference &outer_ref, int32_t field_idx);
	static JoinType TransformJoinType(substrait::JoinRel::JoinType stype, bool lateral_only);
	unique_ptr<ParsedExpression> TransformScalarFunctionExpr(const substrait::Expression &sexpr);
	unique_ptr<ParsedExpression> TransformIfThenExpr(const substrait::Expression &sexpr);
	unique_ptr<ParsedExpression> TransformCastExpr(const substrait::Expression &sexpr);
	unique_ptr<ParsedExpression> TransformInExpr(const substrait::Expression &sexpr);
	unique_ptr<ParsedExpression> TransformNested(const substrait::Expression &sexpr,
	                                             RootNameIterator *iterator = nullptr);

	//! Builds the DuckDB equivalent of a Substrait `extract` call. Throws for components
	//! DuckDB cannot express, rather than emitting SQL that silently means something else.
	static unique_ptr<ParsedExpression> TransformExtractExpr(const vector<string> &enum_expressions,
	                                                        vector<unique_ptr<ParsedExpression>> children);
	static string RemapFunctionName(const string &function_name);
	static string RemoveExtension(const string &function_name);
	static LogicalType SubstraitToDuckType(const substrait::Type &s_type);
	//! Looks up for aggregation function in functions_map
	string FindFunction(uint64_t id);

	//! Transform Substrait Sort Order to DuckDB Order
	OrderByNode TransformOrder(const substrait::SortField &sordf);
	//! DuckDB Client Context
	shared_ptr<ClientContext> context;
	//! CTEs
	vector<shared_ptr<Relation>> ctes;
	//! Substrait Plan
	substrait::Plan plan;
	//! Variable used to register functions
	unordered_map<uint64_t, string> functions_map;
	//! Tracks the left side's column names for each currently-enclosing LateralJoinRel,
	//! keyed by that LateralJoinRel's RelCommon.rel_anchor. Used to resolve OuterReference
	//! field references into a name-based ColumnRefExpression against the "left" alias.
	struct LateralScope {
		uint32_t rel_anchor;
		vector<string> left_column_names;
	};
	vector<LateralScope> lateral_scopes;
	//! Remapped functions with differing names to the correct DuckDB functions
	//! names
	static const unordered_map<std::string, std::string> function_names_remap;
	//! Substrait `extract` component -> DuckDB date_part specifier plus any fix-up needed
	//! to match Substrait's semantics. Absent components are rejected explicitly.
	static const case_insensitive_map_t<SubstraitExtractComponent> extract_components;
	vector<ParsedExpression *> struct_expressions;
	//! If we should acquire a client context lock when creating the relatiosn
	const bool acquire_lock;
};
} // namespace duckdb
