//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "zetasql/local_service/local_service.h"

#include <cstdint>
#include <string>
#include <utility>

#include "zetasql/base/logging.h"
#include "zetasql/base/path.h"
#include "google/protobuf/wrappers.pb.h"
#include "google/protobuf/compiler/importer.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/text_format.h"
#include "zetasql/common/status_payload_utils.h"
#include "zetasql/common/testing/proto_matchers.h"
#include "zetasql/base/testing/status_matchers.h"
#include "zetasql/common/testing/testing_proto_util.h"
#include "zetasql/proto/function.pb.h"
#include "zetasql/proto/simple_catalog.pb.h"
#include "zetasql/public/parse_resume_location.pb.h"
#include "zetasql/public/simple_catalog.h"
#include "zetasql/public/simple_table.pb.h"
#include "zetasql/public/type.h"
#include "zetasql/public/type.pb.h"
#include "zetasql/public/value.h"
#include "zetasql/public/value.pb.h"
#include "zetasql/resolved_ast/resolved_ast.pb.h"
#include "zetasql/testdata/test_proto3.pb.h"
#include "zetasql/testdata/test_schema.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/str_cat.h"
#include "zetasql/base/status.h"

namespace zetasql {

using ::zetasql::testing::EqualsProto;
using ::testing::Not;
using ::zetasql_base::testing::IsOk;
using ::zetasql_base::testing::StatusIs;

namespace local_service {

class ZetaSqlLocalServiceImplTest : public ::testing::Test {
 protected:
  void SetUp() override {
    source_tree_ = CreateProtoSourceTree();
    proto_importer_ = absl::make_unique<google::protobuf::compiler::Importer>(
        source_tree_.get(), nullptr);
    ASSERT_NE(nullptr, proto_importer_->Import(
                           "zetasql/testdata/test_schema.proto"));
    pool_ = absl::make_unique<google::protobuf::DescriptorPool>(proto_importer_->pool());
    EXPECT_EQ(0, service_.NumSavedPreparedExpression());
  }

  void TearDown() override {
    EXPECT_EQ(0, service_.NumSavedPreparedExpression());
  }

  absl::Status Prepare(const PrepareRequest& request,
                       PrepareResponse* response) {
    return service_.Prepare(request, response);
  }

  absl::Status Unprepare(int64_t id) { return service_.Unprepare(id); }

  absl::Status Evaluate(const EvaluateRequest& request,
                        EvaluateResponse* response) {
    return service_.Evaluate(request, response);
  }

  absl::Status Analyze(const AnalyzeRequest& request,
                       AnalyzeResponse* response) {
    return service_.Analyze(request, response);
  }

  absl::Status BuildSql(const BuildSqlRequest& request,
                        BuildSqlResponse* response) {
    return service_.BuildSql(request, response);
  }

  absl::Status ExtractTableNamesFromStatement(
      const ExtractTableNamesFromStatementRequest& request,
      ExtractTableNamesFromStatementResponse* response) {
    return service_.ExtractTableNamesFromStatement(request, response);
  }

  absl::Status ExtractTableNamesFromNextStatement(
      const ExtractTableNamesFromNextStatementRequest& request,
      ExtractTableNamesFromNextStatementResponse* response) {
    return service_.ExtractTableNamesFromNextStatement(request, response);
  }

  absl::Status FormatSql(const FormatSqlRequest& request,
                         FormatSqlResponse* response) {
    return service_.FormatSql(request, response);
  }

  absl::Status UnregisterCatalog(int64_t id) {
    return service_.UnregisterCatalog(id);
  }

  size_t NumSavedPreparedExpression() {
    return service_.NumSavedPreparedExpression();
  }

  absl::Status GetTableFromProto(const TableFromProtoRequest& request,
                                 SimpleTableProto* response) {
    return service_.GetTableFromProto(request, response);
  }

  absl::Status GetBuiltinFunctions(
      const ZetaSQLBuiltinFunctionOptionsProto& proto,
      GetBuiltinFunctionsResponse* response) {
    return service_.GetBuiltinFunctions(proto, response);
  }

  ZetaSqlLocalServiceImpl service_;
  std::unique_ptr<google::protobuf::compiler::DiskSourceTree> source_tree_;
  std::unique_ptr<google::protobuf::compiler::Importer> proto_importer_;
  std::unique_ptr<google::protobuf::DescriptorPool> pool_;
  TypeFactory factory_;
};

TEST_F(ZetaSqlLocalServiceImplTest, PrepareAndCleanup) {
  PrepareRequest request;
  TypeFactory factory;
  FileDescriptorSetMap file_descriptor_set_map;

  // Add a proto-type column.
  const ProtoType* proto_type;
  ZETASQL_ASSERT_OK(factory.MakeProtoType(zetasql_test::KitchenSinkPB::descriptor(),
                                  &proto_type));
  auto* column = request.mutable_options()->add_expression_columns();
  column->set_name("c");
  ZETASQL_ASSERT_OK(proto_type->SerializeToProtoAndDistinctFileDescriptors(
      column->mutable_type(), &file_descriptor_set_map));

  // And add an enum-type param.
  const EnumType* enum_type;
  ZETASQL_ASSERT_OK(factory.MakeEnumType(
      pool_->FindEnumTypeByName("zetasql_test.TestEnum"), &enum_type));
  auto* param = request.mutable_options()->add_query_parameters();
  param->set_name("e");
  ZETASQL_ASSERT_OK(enum_type->SerializeToProtoAndDistinctFileDescriptors(
      param->mutable_type(), &file_descriptor_set_map));

  // The proto and enum come from different descriptor pools, resulting 2 file
  // descriptor sets.
  ASSERT_EQ(2, file_descriptor_set_map.size());

  for (int i = 0; i < file_descriptor_set_map.size(); ++i) {
    request.add_file_descriptor_set();
  }
  for (const auto& pair : file_descriptor_set_map) {
    *request.mutable_file_descriptor_set(pair.second->descriptor_set_index) =
        pair.second->file_descriptor_set;
  }

  // Use the column and param in the expression.
  request.set_sql("IF(c.int32_val=1, @e, null)");

  PrepareResponse wrapper;
  ZETASQL_ASSERT_OK(Prepare(request, &wrapper));

  // Check result type and value.
  PreparedState response = wrapper.prepared();
  EXPECT_EQ(TYPE_ENUM, response.output_type().type_kind());
  EXPECT_EQ("zetasql_test.TestEnum",
            response.output_type().enum_type().enum_name());
  EXPECT_EQ(0, response.output_type().file_descriptor_set_size());
  EXPECT_EQ(1, response.output_type().enum_type().file_descriptor_set_index());

  // Check prepared id.
  EXPECT_TRUE(response.has_prepared_expression_id());
  EXPECT_EQ(1, NumSavedPreparedExpression());

  // Check column.
  EXPECT_EQ(1, response.referenced_columns_size());
  EXPECT_EQ("c", response.referenced_columns(0));

  // Check named parameter.
  EXPECT_EQ(1, response.referenced_parameters_size());
  EXPECT_EQ("e", response.referenced_parameters(0));

  // Check positional parameter.
  EXPECT_EQ(0, response.positional_parameter_count());

  // Cleanup
  ZETASQL_ASSERT_OK(Unprepare(response.prepared_expression_id()));
}

TEST_F(ZetaSqlLocalServiceImplTest, PrepareFailuresNoRegister) {
  PrepareRequest request;
  PrepareResponse response;

  request.set_sql("foo");
  ASSERT_FALSE(Prepare(request, &response).ok());
  // No prepared state saved on failure.
  EXPECT_EQ(0, NumSavedPreparedExpression());

  request.set_sql("foo + @bar");
  auto* param = request.mutable_options()->add_query_parameters();
  param->set_name("bar");
  param->mutable_type()->set_type_kind(TYPE_INT64);

  ASSERT_FALSE(Prepare(request, &response).ok());
  EXPECT_EQ(0, NumSavedPreparedExpression());

  auto* column = request.mutable_options()->add_expression_columns();
  column->set_name("foo");
  column->mutable_type()->set_type_kind(TYPE_STRING);

  ASSERT_FALSE(Prepare(request, &response).ok());
  EXPECT_EQ(0, NumSavedPreparedExpression());
}

TEST_F(ZetaSqlLocalServiceImplTest, Evaluate) {
  EvaluateRequest request;

  TypeFactory factory;
  FileDescriptorSetMap file_descriptor_set_map;

  // Add a proto-type column.
  const ProtoType* proto_type;
  ZETASQL_ASSERT_OK(factory.MakeProtoType(zetasql_test::KitchenSinkPB::descriptor(),
                                  &proto_type));
  auto* column_type = request.mutable_options()->add_expression_columns();
  column_type->set_name("C");
  ZETASQL_ASSERT_OK(proto_type->SerializeToProtoAndDistinctFileDescriptors(
      column_type->mutable_type(), &file_descriptor_set_map));
  auto* column = request.add_columns();
  column->set_name("C");
  zetasql_test::KitchenSinkPB pb;
  pb.set_int32_val(1);
  pb.set_int64_key_1(2);
  pb.set_int64_key_2(3);
  Value proto_value = values::Proto(proto_type, pb);
  ValueProto proto_pb;
  ZETASQL_ASSERT_OK(proto_value.Serialize(&proto_pb));
  *column->mutable_value() = proto_pb;

  // And add an enum-type param.
  const EnumType* enum_type;
  ZETASQL_ASSERT_OK(factory.MakeEnumType(
      pool_->FindEnumTypeByName("zetasql_test.TestEnum"), &enum_type));
  auto* param_type = request.mutable_options()->add_query_parameters();
  param_type->set_name("e");
  ZETASQL_ASSERT_OK(enum_type->SerializeToProtoAndDistinctFileDescriptors(
      param_type->mutable_type(), &file_descriptor_set_map));
  auto* param = request.add_params();
  param->set_name("e");
  Value enum_value = values::Enum(enum_type, 1);
  ValueProto enum_pb;
  ZETASQL_ASSERT_OK(enum_value.Serialize(&enum_pb));
  *param->mutable_value() = enum_pb;

  // The proto and enum come from different descriptor pools, resulting 2 file
  // descriptor sets.
  ASSERT_EQ(2, file_descriptor_set_map.size());

  for (int i = 0; i < file_descriptor_set_map.size(); ++i) {
    request.add_file_descriptor_set();
  }
  for (const auto& pair : file_descriptor_set_map) {
    *request.mutable_file_descriptor_set(pair.second->descriptor_set_index) =
        pair.second->file_descriptor_set;
  }

  // Use the column and param in the expression.
  request.set_sql("IF(C.int32_val=1, @e, null)");

  EvaluateResponse response;
  ZETASQL_ASSERT_OK(Evaluate(request, &response));

  // check result type and value.
  EXPECT_EQ(TYPE_ENUM, response.prepared().output_type().type_kind());
  EXPECT_EQ("zetasql_test.TestEnum",
            response.prepared().output_type().enum_type().enum_name());
  EXPECT_EQ(0, response.prepared().output_type().file_descriptor_set_size());
  EXPECT_EQ(1, response.prepared()
                   .output_type()
                   .enum_type()
                   .file_descriptor_set_index());
  EXPECT_EQ(1, response.value().enum_value());
  EXPECT_EQ(1, NumSavedPreparedExpression());
  ZETASQL_ASSERT_OK(Unprepare(response.prepared().prepared_expression_id()));
}

TEST_F(ZetaSqlLocalServiceImplTest, EvaluatePrepared) {
  PrepareRequest request;
  TypeFactory factory;
  FileDescriptorSetMap file_descriptor_set_map;

  // Add a proto-type column.
  const ProtoType* proto_type;
  ZETASQL_ASSERT_OK(factory.MakeProtoType(zetasql_test::KitchenSinkPB::descriptor(),
                                  &proto_type));
  auto* column = request.mutable_options()->add_expression_columns();
  column->set_name("c");
  ZETASQL_ASSERT_OK(proto_type->SerializeToProtoAndDistinctFileDescriptors(
      column->mutable_type(), &file_descriptor_set_map));

  // And add an enum-type param.
  const EnumType* enum_type;
  ZETASQL_ASSERT_OK(factory.MakeEnumType(
      pool_->FindEnumTypeByName("zetasql_test.TestEnum"), &enum_type));
  auto* param = request.mutable_options()->add_query_parameters();
  param->set_name("e");
  ZETASQL_ASSERT_OK(enum_type->SerializeToProtoAndDistinctFileDescriptors(
      param->mutable_type(), &file_descriptor_set_map));

  // The proto and enum come from different descriptor pools, resulting 2 file
  // descriptor sets.
  ASSERT_EQ(2, file_descriptor_set_map.size());

  for (int i = 0; i < file_descriptor_set_map.size(); ++i) {
    request.add_file_descriptor_set();
  }
  for (const auto& pair : file_descriptor_set_map) {
    *request.mutable_file_descriptor_set(pair.second->descriptor_set_index) =
        pair.second->file_descriptor_set;
  }

  // Use the column and param in the expression.
  request.set_sql("IF(c.int32_val=1, @e, null)");

  PrepareResponse response;
  ZETASQL_ASSERT_OK(Prepare(request, &response));

  EvaluateRequest evaluate_request;
  evaluate_request.set_prepared_expression_id(
      response.prepared().prepared_expression_id());

  auto* evaluate_column = evaluate_request.add_columns();
  evaluate_column->set_name("c");
  zetasql_test::KitchenSinkPB pb;
  pb.set_int32_val(1);
  pb.set_int64_key_1(2);
  pb.set_int64_key_2(3);
  Value proto_value = values::Proto(proto_type, pb);
  ValueProto proto_pb;
  ZETASQL_ASSERT_OK(proto_value.Serialize(&proto_pb));
  *evaluate_column->mutable_value() = proto_pb;

  // And add an enum-type param.
  auto* evaluate_param = evaluate_request.add_params();
  evaluate_param->set_name("e");
  Value enum_value = values::Enum(enum_type, 1);
  ValueProto enum_pb;
  ZETASQL_ASSERT_OK(enum_value.Serialize(&enum_pb));
  *evaluate_param->mutable_value() = enum_pb;

  EvaluateResponse evaluate_response;
  ZETASQL_ASSERT_OK(Evaluate(evaluate_request, &evaluate_response));

  // Check result value, ensure extra data not sent.
  EXPECT_EQ(1, evaluate_response.value().enum_value());
  EXPECT_EQ(evaluate_response.has_prepared(), false);

  EXPECT_EQ(1, NumSavedPreparedExpression());
  ZETASQL_ASSERT_OK(Unprepare(response.prepared().prepared_expression_id()));
}

TEST_F(ZetaSqlLocalServiceImplTest, EvaluateWithWrongId) {
  EvaluateRequest evaluate_request;
  evaluate_request.set_prepared_expression_id(12345);

  EvaluateResponse evaluate_response;
  ASSERT_FALSE(Evaluate(evaluate_request, &evaluate_response).ok());
  // No prepared state saved on failure.
  EXPECT_EQ(0, NumSavedPreparedExpression());
}

TEST_F(ZetaSqlLocalServiceImplTest, EvaluateFailuresNoRegister) {
  EvaluateRequest request;
  EvaluateResponse response;

  request.set_sql("foo");
  ASSERT_FALSE(Evaluate(request, &response).ok());
  // No prepared state saved on failure.
  EXPECT_EQ(0, NumSavedPreparedExpression());

  request.set_sql("foo + @bar");
  auto* param = request.add_params();
  param->set_name("bar");
  param->mutable_value()->set_int64_value(1);

  ASSERT_FALSE(Evaluate(request, &response).ok());
  EXPECT_EQ(0, NumSavedPreparedExpression());

  auto* column = request.add_columns();
  column->set_name("foo");
  column->mutable_value()->set_string_value("");

  ASSERT_FALSE(Evaluate(request, &response).ok());
  EXPECT_EQ(0, NumSavedPreparedExpression());
}

TEST_F(ZetaSqlLocalServiceImplTest, UnprepareUnknownId) {
  ASSERT_FALSE(Unprepare(10086).ok());
}

TEST_F(ZetaSqlLocalServiceImplTest, TableFromProto) {
  TableFromProtoRequest request;
  SimpleTableProto response;
  const ProtoType* proto_type;
  TypeFactory factory;
  TypeProto proto;
  google::protobuf::FileDescriptorSet descriptor_set;

  ZETASQL_CHECK_OK(factory.MakeProtoType(zetasql_test::TestSQLTable::descriptor(),
                                 &proto_type));
  ZETASQL_CHECK_OK(
      proto_type->SerializeToProtoAndFileDescriptors(&proto, &descriptor_set));
  *request.mutable_file_descriptor_set() = descriptor_set;
  *request.mutable_proto() = proto.proto_type();
  ASSERT_TRUE(GetTableFromProto(request, &response).ok());
  EXPECT_EQ("TestSQLTable", response.name());
  EXPECT_FALSE(response.is_value_table());
  EXPECT_EQ(2, response.column_size());
  const SimpleColumnProto column1 = response.column(0);
  EXPECT_EQ("f1", column1.name());
  EXPECT_EQ(TYPE_INT32, column1.type().type_kind());
  const SimpleColumnProto column2 = response.column(1);
  EXPECT_EQ("f2", column2.name());
  EXPECT_EQ(TYPE_INT32, column2.type().type_kind());
}

TEST_F(ZetaSqlLocalServiceImplTest, NonZetaSQLTableFromProto) {
  TableFromProtoRequest request;
  SimpleTableProto response;
  const ProtoType* proto_type;
  TypeFactory factory;
  ZETASQL_CHECK_OK(factory.MakeProtoType(zetasql_test::KitchenSinkPB::descriptor(),
                                 &proto_type));
  TypeProto proto;
  google::protobuf::FileDescriptorSet descriptor_set;
  ZETASQL_CHECK_OK(
      proto_type->SerializeToProtoAndFileDescriptors(&proto, &descriptor_set));
  *request.mutable_file_descriptor_set() = descriptor_set;
  *request.mutable_proto() = proto.proto_type();
  ASSERT_TRUE(GetTableFromProto(request, &response).ok());
  SimpleTableProto expected;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(R"pb(
    name: "KitchenSinkPB"
    is_value_table: true
    column {
      name: "value"
      type {
        type_kind: TYPE_PROTO
        proto_type {
          proto_name: "zetasql_test.KitchenSinkPB"
          proto_file_name: "zetasql/testdata/test_schema.proto"
        }
      }
      is_pseudo_column: false
    }
  )pb", &expected));
  EXPECT_EQ(expected.DebugString(), response.DebugString());
}

TEST_F(ZetaSqlLocalServiceImplTest, BadTableFromProto) {
  TableFromProtoRequest request;
  SimpleTableProto response;
  const ProtoType* proto_type;
  TypeFactory factory;
  ZETASQL_CHECK_OK(factory.MakeProtoType(zetasql_test::KitchenSinkPB::descriptor(),
                                 &proto_type));
  TypeProto proto;
  google::protobuf::FileDescriptorSet descriptor_set;
  ZETASQL_CHECK_OK(
      proto_type->SerializeToProtoAndFileDescriptors(&proto, &descriptor_set));
  *request.mutable_proto() = proto.proto_type();
  absl::Status status = GetTableFromProto(request, &response);
  ASSERT_FALSE(status.ok());
  EXPECT_EQ("Proto type name not found: zetasql_test.KitchenSinkPB",
            status.message());
  EXPECT_TRUE(response.DebugString().empty());

  request.mutable_proto()->set_proto_file_name("unmatched_file_name");
  *request.mutable_file_descriptor_set() = descriptor_set;
  status = GetTableFromProto(request, &response);
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(absl::StrCat("Proto zetasql_test.KitchenSinkPB found in ",
                         "zetasql/testdata/test_schema.proto",
                         ", not unmatched_file_name as specified."),
            status.message());
}

TEST_F(ZetaSqlLocalServiceImplTest, AnalyzeWrongCatalogId) {
  AnalyzeRequest request;
  request.set_registered_catalog_id(12345);

  AnalyzeResponse response;
  absl::Status status = Analyze(request, &response);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ("generic::invalid_argument: Registered catalog 12345 unknown.",
            internal::StatusToString(status));
}

TEST_F(ZetaSqlLocalServiceImplTest, ExtractTableNamesFromStatement) {
  ExtractTableNamesFromStatementRequest request;
  request.set_sql_statement("select count(1) from foo.bar;");

  ExtractTableNamesFromStatementResponse response;
  ZETASQL_ASSERT_OK(ExtractTableNamesFromStatement(request, &response));
  ExtractTableNamesFromStatementResponse expectedResponse;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(R"pb(table_name {
                                                    table_name_segment: "foo"
                                                    table_name_segment: "bar"
                                                  })pb", &expectedResponse));
  EXPECT_THAT(response, EqualsProto(expectedResponse));
}

TEST_F(ZetaSqlLocalServiceImplTest, ExtractTableNamesFromScript) {
  ExtractTableNamesFromStatementRequest request;
  request.set_sql_statement(
      "select count(1) from foo.bar; select count(1) from x.y.z");
  request.set_allow_script(true);
  ExtractTableNamesFromStatementResponse response;
  ZETASQL_ASSERT_OK(ExtractTableNamesFromStatement(request, &response));
  ExtractTableNamesFromStatementResponse expectedResponse;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(table_name { table_name_segment: "foo" table_name_segment: "bar" }
           table_name {
             table_name_segment: "x"
             table_name_segment: "y"
             table_name_segment: "z"
           })pb",
      &expectedResponse));
  EXPECT_THAT(response, EqualsProto(expectedResponse));
}

TEST_F(ZetaSqlLocalServiceImplTest, ExtractTableNamesFromFirstStatement) {
  ExtractTableNamesFromNextStatementRequest request;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(parse_resume_location {
             input: "select count(1) from foo.bar; select id from baz;"
           })pb",
      &request));

  ExtractTableNamesFromNextStatementResponse response;
  ZETASQL_ASSERT_OK(ExtractTableNamesFromNextStatement(request, &response));
  ExtractTableNamesFromNextStatementResponse expectedResponse;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(table_name {
             table_name_segment: "foo"
             table_name_segment: "bar"
           }
           resume_byte_position: 29)pb",
      &expectedResponse));
  EXPECT_THAT(response, EqualsProto(expectedResponse));
}

TEST_F(ZetaSqlLocalServiceImplTest, ExtractWithUnsupportedStatement) {
  ExtractTableNamesFromNextStatementRequest request;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(parse_resume_location {
             input: "CREATE TABLE test AS SELECT COUNT(1) FROM foo.bar;"
             byte_position: 0
           })pb",
      &request));

  ExtractTableNamesFromNextStatementResponse response;
  absl::Status status = ExtractTableNamesFromNextStatement(request, &response);
  EXPECT_THAT(status,
              ::zetasql_base::testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(ZetaSqlLocalServiceImplTest, ExtractWithWrongStatementSupported) {
  ExtractTableNamesFromNextStatementRequest request;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(parse_resume_location {
             input: "CREATE TABLE test AS SELECT COUNT(1) FROM foo.bar;"
             byte_position: 0
           }
           options {
             supported_statement_kinds: RESOLVED_CONSTANT
           }
           )pb",
      &request));

  ExtractTableNamesFromNextStatementResponse response;
  absl::Status status = ExtractTableNamesFromNextStatement(request, &response);
  EXPECT_THAT(status,
              ::zetasql_base::testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(ZetaSqlLocalServiceImplTest, ExtractWithAllStatementsSupported) {
  ExtractTableNamesFromNextStatementRequest request;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(parse_resume_location {
             input: "CREATE TABLE test AS SELECT COUNT(1) FROM foo.bar;"
             byte_position: 0
           }
           options {}
           )pb",
      &request));

  ExtractTableNamesFromNextStatementResponse response;
  ZETASQL_ASSERT_OK(ExtractTableNamesFromNextStatement(request, &response));
  ExtractTableNamesFromNextStatementResponse expectedResponse;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(table_name {
             table_name_segment: "foo"
             table_name_segment: "bar"
           }
           resume_byte_position: 50)pb",
      &expectedResponse));
  EXPECT_THAT(response, EqualsProto(expectedResponse));
}

TEST_F(ZetaSqlLocalServiceImplTest, ExtractTableNamesFromEmptyStatement) {
  ExtractTableNamesFromNextStatementRequest request;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(parse_resume_location {
             input: "select count(1) from foo.bar;   "
             byte_position: 29
           })pb",
      &request));

  ExtractTableNamesFromNextStatementResponse response;
  absl::Status status = ExtractTableNamesFromNextStatement(request, &response);
  ASSERT_FALSE(status.ok());
  EXPECT_EQ("Syntax error: Unexpected end of statement [at 1:30]",
            status.message());
}

TEST_F(ZetaSqlLocalServiceImplTest, ExtractWithBigResumePosition) {
  ExtractTableNamesFromNextStatementRequest request;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(parse_resume_location {
             input: "select count(1) from foo.bar;"
             byte_position: 9000
           })pb",
      &request));

  ExtractTableNamesFromNextStatementResponse response;
  absl::Status status = ExtractTableNamesFromNextStatement(request, &response);
  ASSERT_FALSE(status.ok());
  EXPECT_THAT(status, ::zetasql_base::testing::StatusIs(absl::StatusCode::kInternal));
}

TEST_F(ZetaSqlLocalServiceImplTest, ExtractWithNegativeResumePosition) {
  ExtractTableNamesFromNextStatementRequest request;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(parse_resume_location {
             input: "select count(1) from foo.bar;"
             byte_position: -1
           })pb",
      &request));

  ExtractTableNamesFromNextStatementResponse response;
  absl::Status status = ExtractTableNamesFromNextStatement(request, &response);
  ASSERT_FALSE(status.ok());
  EXPECT_THAT(status, ::zetasql_base::testing::StatusIs(absl::StatusCode::kInternal));
}

TEST_F(ZetaSqlLocalServiceImplTest, ExtractTableNamesFromNextStatement) {
  ExtractTableNamesFromNextStatementRequest request;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(parse_resume_location {
             input: "select count(1) from foo.bar; select id from baz;"
             byte_position: 29
           })pb",
      &request));

  ExtractTableNamesFromNextStatementResponse response;
  ZETASQL_ASSERT_OK(ExtractTableNamesFromNextStatement(request, &response));
  ExtractTableNamesFromNextStatementResponse expectedResponse;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(table_name {
             table_name_segment: "baz"
           }
           resume_byte_position: 49)pb",
      &expectedResponse));
  EXPECT_THAT(response, EqualsProto(expectedResponse));
}

TEST_F(ZetaSqlLocalServiceImplTest, ExtractTableNamesWithNoSemicolon) {
  ExtractTableNamesFromNextStatementRequest request;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(parse_resume_location {
             input: "select count(1) from foo.bar; select id from baz"
             byte_position: 29
           })pb",
      &request));

  ExtractTableNamesFromNextStatementResponse response;
  ZETASQL_ASSERT_OK(ExtractTableNamesFromNextStatement(request, &response));
  ExtractTableNamesFromNextStatementResponse expectedResponse;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(table_name {
             table_name_segment: "baz"
           }
           resume_byte_position: 48)pb",
      &expectedResponse));
  EXPECT_THAT(response, EqualsProto(expectedResponse));
}

TEST_F(ZetaSqlLocalServiceImplTest, UnregisterWrongCatalogId) {
  absl::Status status = UnregisterCatalog(12345);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ("generic::invalid_argument: Unknown catalog ID: 12345",
            internal::StatusToString(status));
}

TEST_F(ZetaSqlLocalServiceImplTest, Analyze) {
  const std::string catalog_proto_text = R"pb(
    name: "foo"
    table {
      name: "bar"
      serialization_id: 1
      column {
        name: "baz"
        type { type_kind: TYPE_INT32 }
        is_pseudo_column: false
      }
    })pb";

  SimpleCatalogProto catalog;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(catalog_proto_text, &catalog));

  AnalyzeRequest request;
  *request.mutable_simple_catalog() = catalog;
  request.set_sql_statement("select baz from bar;");

  AnalyzeResponse response;
  ZETASQL_EXPECT_OK(Analyze(request, &response));

  AnalyzeRequest request2;
  *request2.mutable_simple_catalog() = catalog;
  ParseResumeLocationProto* parse_resume_location2 =
      request2.mutable_parse_resume_location();
  parse_resume_location2->set_input("select baz from bar;select baz from bar;");
  parse_resume_location2->set_byte_position(0);
  AnalyzeResponse response2;
  ZETASQL_EXPECT_OK(Analyze(request2, &response2));
  EXPECT_EQ(20, response2.resume_byte_position());

  AnalyzeRequest request3;
  *request3.mutable_simple_catalog() = catalog;
  ParseResumeLocationProto* parse_resume_location3 =
      request3.mutable_parse_resume_location();
  parse_resume_location3->set_input("select baz from bar;select baz from bar;");
  parse_resume_location3->set_byte_position(response2.resume_byte_position());
  AnalyzeResponse response3;
  ZETASQL_EXPECT_OK(Analyze(request3, &response3));
  EXPECT_EQ(40, response3.resume_byte_position());
}

void AddEmptyFileDescriptorSet(DescriptorPoolListProto* list) {
  list->add_definitions()->mutable_file_descriptor_set();
}

void AddBuiltin(DescriptorPoolListProto* list) {
  list->add_definitions()->mutable_builtin();
}

void AddKitchenSinkDescriptorPool(DescriptorPoolListProto* list) {
  TypeFactory factory;
  AnalyzerOptions options;
  const ProtoType* proto_type = nullptr;
  ZETASQL_CHECK_OK(factory.MakeProtoType(zetasql_test::KitchenSinkPB::descriptor(),
                                 &proto_type));
  TypeProto ignored;

  ZETASQL_CHECK_OK(proto_type->SerializeToProtoAndFileDescriptors(
      &ignored, list->add_definitions()->mutable_file_descriptor_set()));
}

void AddKitchenSink3DescriptorPool(DescriptorPoolListProto* list) {
  TypeFactory factory;
  AnalyzerOptions options;
  const ProtoType* proto_type = nullptr;
  ZETASQL_CHECK_OK(factory.MakeProtoType(
      zetasql_test::Proto3KitchenSink::descriptor(), &proto_type));
  TypeProto ignored;

  ZETASQL_CHECK_OK(proto_type->SerializeToProtoAndFileDescriptors(
      &ignored, list->add_definitions()->mutable_file_descriptor_set()));
}

void AddDateTruncToCatalog(SimpleCatalogProto* catalog) {
  catalog->mutable_builtin_function_options()->add_include_function_ids(
      FN_DATE_TRUNC_DATE);
}

// builtin-only use for datetimepart
TEST_F(ZetaSqlLocalServiceImplTest,
       AnalyzeWithDescriptorPoolListProtoBuiltinOnlyForFunction) {
  AnalyzeRequest request;
  request.set_sql_statement(R"(select DATE_TRUNC(DATE "2020-10-20", MONTH))");
  // We add a useless descriptor set, this ensures that 'zero' is not somehow
  // magic.
  AddEmptyFileDescriptorSet(request.mutable_descriptor_pool_list());
  AddBuiltin(request.mutable_descriptor_pool_list());
  AddDateTruncToCatalog(request.mutable_simple_catalog());

  AnalyzeResponse response;
  ZETASQL_EXPECT_OK(Analyze(request, &response));

  auto date_trunc_call = response.resolved_statement()
                             .resolved_query_stmt_node()
                             .query()
                             .resolved_project_scan_node()
                             .expr_list(0)
                             .expr()
                             .resolved_function_call_base_node()
                             .resolved_function_call_node()
                             .parent();
  auto signature = date_trunc_call.signature();
  EXPECT_EQ(date_trunc_call.function().name(), "ZetaSQL:date_trunc");

  TypeProto datetimepart_type;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(type_kind: TYPE_ENUM
           enum_type {
             enum_name: "zetasql.functions.DateTimestampPart"
             enum_file_name: "zetasql/public/functions/datetime.proto"
             file_descriptor_set_index: 1
           })pb",
      &datetimepart_type));

  EXPECT_THAT(signature.argument(1).type(), EqualsProto(datetimepart_type));
}

// The presence of the built in DescriptorPool doesn't magically make that
// available in the catalog
TEST_F(ZetaSqlLocalServiceImplTest,
       AnalyzeWithDescriptorPoolListProtoBuiltinNotMagicallyInCatalog) {
  AnalyzeRequest request;
  request.set_sql_statement(R"(select new google.protobuf.Int64Value())");
  AddBuiltin(request.mutable_descriptor_pool_list());

  AnalyzeResponse response;
  EXPECT_THAT(Analyze(request, &response),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(ZetaSqlLocalServiceImplTest,
       AnalyzeWithDescriptorPoolListProtoCatalogIndependence) {
  constexpr int32_t kKitchenSinkPool = 1;
  constexpr int32_t kKitchenSink3Pool = 2;
  AnalyzeRequest request;
  request.set_sql_statement(
      R"(select new zetasql_test.KitchenSinkPB(1 as int64_key_1, 2 as int64_key_2),
                new a.zetasql_test.Proto3KitchenSink(1 as int64_val),
                new b.zetasql_test.KitchenSinkPB(1 as int64_key_1, 2 as int64_key_2))");
  AddBuiltin(request.mutable_descriptor_pool_list());
  // kKitchenSinkPool ->
  AddKitchenSinkDescriptorPool(request.mutable_descriptor_pool_list());
  // kKitchenSink3Pool ->
  AddKitchenSink3DescriptorPool(request.mutable_descriptor_pool_list());

  SimpleCatalogProto* root_catalog = request.mutable_simple_catalog();
  root_catalog->set_file_descriptor_set_index(kKitchenSinkPool);

  SimpleCatalogProto* sub_catalog_a = root_catalog->add_catalog();
  sub_catalog_a->set_name("a");
  sub_catalog_a->set_file_descriptor_set_index(kKitchenSink3Pool);

  SimpleCatalogProto* sub_catalog_b = root_catalog->add_catalog();
  sub_catalog_b->set_name("b");
  sub_catalog_b->set_file_descriptor_set_index(kKitchenSinkPool);

  AnalyzeResponse response;
  ZETASQL_CHECK_OK(Analyze(request, &response));

  TypeProto expected_kitchen_sink_proto_type;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        type_kind: TYPE_PROTO
        proto_type {
          proto_name: "zetasql_test.KitchenSinkPB"
          proto_file_name: "zetasql/testdata/test_schema.proto"
          file_descriptor_set_index: 1
        })pb",
      &expected_kitchen_sink_proto_type));

  TypeProto expected_kitchen_sink_proto3_type;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        type_kind: TYPE_PROTO
        proto_type {
          proto_name: "zetasql_test.Proto3KitchenSink"
          proto_file_name: "zetasql/testdata/test_proto3.proto"
          file_descriptor_set_index: 2
        })pb",
      &expected_kitchen_sink_proto3_type));

  ZETASQL_LOG(INFO) << "response\n" << response.DebugString();
  auto output_columns = response.resolved_statement()
                            .resolved_query_stmt_node()
                            .output_column_list();
  ASSERT_EQ(output_columns.size(), 3);
  EXPECT_THAT(output_columns[0].column().type(),
              EqualsProto(expected_kitchen_sink_proto_type));

  EXPECT_THAT(output_columns[1].column().type(),
              EqualsProto(expected_kitchen_sink_proto3_type));

  EXPECT_THAT(output_columns[2].column().type(),
              EqualsProto(expected_kitchen_sink_proto_type));
}

// We can use the builtin DescriptorPool for parameters, even if that
// pool is not used in the catalog (or there is no catalog).
TEST_F(ZetaSqlLocalServiceImplTest,
       AnalyzeWithDescriptorPoolListProtoBuiltinOnlyForParameter) {
  AnalyzeRequest request;
  request.set_sql_statement(R"(select @p1.value, @p1)");
  // We add a useless descriptor set, this ensures that 'zero' is not somehow
  // magic.
  AddEmptyFileDescriptorSet(request.mutable_descriptor_pool_list());
  AddBuiltin(request.mutable_descriptor_pool_list());

  TypeFactory factory;
  AnalyzerOptions options;
  const ProtoType* int64_proto_type = nullptr;
  ZETASQL_CHECK_OK(factory.MakeProtoType(google::protobuf::Int64Value::descriptor(),
                                 &int64_proto_type));
  ZETASQL_CHECK_OK(options.AddQueryParameter("p1", int64_proto_type));

  google::protobuf::DescriptorPool empty;
  FileDescriptorSetMap descriptor_map;
  // Add an empty entry to ensure 'zero' is not magic, this won't be used.
  descriptor_map.emplace(&empty, std::make_unique<Type::FileDescriptorEntry>());
  ZETASQL_CHECK_OK(options.Serialize(&descriptor_map, request.mutable_options()));
  // We expect the generated pool (i.e. what Int64Value is using) as the 2nd
  // entry.
  ZETASQL_CHECK_EQ(descriptor_map.size(), 2);
  EXPECT_THAT(descriptor_map[&empty]->file_descriptors, ::testing::IsEmpty());

  AnalyzeResponse response;
  ZETASQL_EXPECT_OK(Analyze(request, &response));
  TypeProto expected_proto_type;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        type_kind: TYPE_PROTO
        proto_type {
          proto_name: "google.protobuf.Int64Value"
          proto_file_name: "google/protobuf/wrappers.proto"
          file_descriptor_set_index: 1
        })pb",
      &expected_proto_type));
  auto output_columns = response.resolved_statement()
                            .resolved_query_stmt_node()
                            .output_column_list();
  ASSERT_EQ(output_columns.size(), 2);
  // Note, this is zetasql::TypeKind
  EXPECT_EQ(output_columns[0].column().type().type_kind(), TYPE_INT64);
  // Note, this is a representation of google.protobuf.Int64Value.
  EXPECT_THAT(output_columns[1].column().type(),
              EqualsProto(expected_proto_type));
}

TEST_F(ZetaSqlLocalServiceImplTest, AnalyzeExpression) {
  SimpleCatalogProto catalog;

  zetasql::ZetaSQLBuiltinFunctionOptionsProto options;
  zetasql::ZetaSQLBuiltinFunctionOptionsProto* builtin_function_options =
      catalog.mutable_builtin_function_options();
  *builtin_function_options = options;

  AnalyzeRequest request;
  *request.mutable_simple_catalog() = catalog;
  request.set_sql_expression("123");

  AnalyzeResponse response;
  ZETASQL_EXPECT_OK(Analyze(request, &response));

  AnalyzeResponse expectedResponse;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(resolved_expression {
             resolved_literal_node {
               parent {
                 type { type_kind: TYPE_INT64 }
                 type_annotation_map {}
               }
               value {
                 type { type_kind: TYPE_INT64 }
                 value { int64_value: 123 }
               }
               has_explicit_type: false
               float_literal_id: 0
               preserve_in_literal_remover: false
             }
           })pb",
      &expectedResponse));
  EXPECT_THAT(response, EqualsProto(expectedResponse));

  AnalyzeRequest request2;
  *request2.mutable_simple_catalog() = catalog;

  request2.set_sql_expression("foo < 123");
  auto* column = request2.mutable_options()->add_expression_columns();
  column->set_name("foo");
  column->mutable_type()->set_type_kind(TYPE_INT32);
  AnalyzeResponse response2;
  ZETASQL_EXPECT_OK(Analyze(request2, &response2));

  AnalyzeResponse expectedResponse2;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(resolved_expression {
             resolved_function_call_base_node {
               resolved_function_call_node {
                 parent {
                   parent {
                     type { type_kind: TYPE_BOOL }
                     type_annotation_map {}
                   }
                   function { name: "ZetaSQL:$less" }
                   signature {
                     argument {
                       kind: ARG_TYPE_FIXED
                       type { type_kind: TYPE_INT32 }
                       options {
                         cardinality: REQUIRED
                         extra_relation_input_columns_allowed: true
                       }
                       num_occurrences: 1
                     }
                     argument {
                       kind: ARG_TYPE_FIXED
                       type { type_kind: TYPE_INT32 }
                       options {
                         cardinality: REQUIRED
                         extra_relation_input_columns_allowed: true
                       }
                       num_occurrences: 1
                     }
                     return_type {
                       kind: ARG_TYPE_FIXED
                       type { type_kind: TYPE_BOOL }
                       options {
                         cardinality: REQUIRED
                         extra_relation_input_columns_allowed: true
                       }
                       num_occurrences: 1
                     }
                     context_id: 105
                     options { is_deprecated: false }
                   }
                   argument_list {
                     resolved_expression_column_node {
                       parent {
                         type { type_kind: TYPE_INT32 }
                         type_annotation_map {}
                       }
                       name: "foo"
                     }
                   }
                   argument_list {
                     resolved_literal_node {
                       parent {
                         type { type_kind: TYPE_INT32 }
                         type_annotation_map {}
                       }
                       value {
                         type { type_kind: TYPE_INT32 }
                         value { int32_value: 123 }
                       }
                       has_explicit_type: false
                       float_literal_id: 0
                       preserve_in_literal_remover: false
                     }
                   }
                   error_mode: DEFAULT_ERROR_MODE
                 }
                 function_call_info {}
               }
             }
           })pb",
      &expectedResponse2));
  EXPECT_THAT(response2, EqualsProto(expectedResponse2));
}

TEST_F(ZetaSqlLocalServiceImplTest, BuildSqlStatement) {
  const std::string catalog_proto_text = R"pb(
    name: "foo"
    table {
      name: "bar"
      serialization_id: 1
      column {
        name: "baz"
        type { type_kind: TYPE_INT32 }
        is_pseudo_column: false
      }
    })pb";

  SimpleCatalogProto catalog;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(catalog_proto_text, &catalog));

  BuildSqlRequest request;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(resolved_statement {
             resolved_query_stmt_node {
               output_column_list {
                 name: "baz"
                 column {
                   column_id: 1
                   table_name: "bar"
                   name: "baz"
                   type { type_kind: TYPE_INT32 }
                 }
               }
               is_value_table: false
               query {
                 resolved_project_scan_node {
                   parent {
                     column_list {
                       column_id: 1
                       table_name: "bar"
                       name: "baz"
                       type { type_kind: TYPE_INT32 }
                     }
                     is_ordered: false
                   }
                   input_scan {
                     resolved_table_scan_node {
                       parent {
                         column_list {
                           column_id: 1
                           table_name: "bar"
                           name: "baz"
                           type { type_kind: TYPE_INT32 }
                         }
                         is_ordered: false
                       }
                       table {
                         name: "bar"
                         serialization_id: 1
                         full_name: "bar"
                       }
                       column_index_list: 0
                       alias: ""
                     }
                   }
                 }
               }
             }
           })pb",
      &request));

  *request.mutable_simple_catalog() = catalog;

  BuildSqlResponse response;
  ZETASQL_EXPECT_OK(BuildSql(request, &response));

  BuildSqlResponse expectedResponse;
  expectedResponse.set_sql(
      "SELECT bar_2.a_1 AS baz FROM (SELECT bar.baz AS a_1 FROM bar) AS bar_2");

  EXPECT_THAT(response, EqualsProto(expectedResponse));
}

TEST_F(ZetaSqlLocalServiceImplTest, BuildSqlExpression) {
  BuildSqlRequest request;
  ZETASQL_CHECK(google::protobuf::TextFormat::ParseFromString(
      R"pb(resolved_expression {
             resolved_function_call_base_node {
               resolved_function_call_node {
                 parent {
                   parent { type { type_kind: TYPE_BOOL } }
                   function { name: "ZetaSQL:$less" }
                   signature {
                     argument {
                       kind: ARG_TYPE_FIXED
                       type { type_kind: TYPE_INT32 }
                       options {
                         cardinality: REQUIRED
                         extra_relation_input_columns_allowed: true
                       }
                       num_occurrences: 1
                     }
                     argument {
                       kind: ARG_TYPE_FIXED
                       type { type_kind: TYPE_INT32 }
                       options {
                         cardinality: REQUIRED
                         extra_relation_input_columns_allowed: true
                       }
                       num_occurrences: 1
                     }
                     return_type {
                       kind: ARG_TYPE_FIXED
                       type { type_kind: TYPE_BOOL }
                       options {
                         cardinality: REQUIRED
                         extra_relation_input_columns_allowed: true
                       }
                       num_occurrences: 1
                     }
                     context_id: 105
                     options { is_deprecated: false }
                   }
                   argument_list {
                     resolved_expression_column_node {
                       parent { type { type_kind: TYPE_INT32 } }
                       name: "foo"
                     }
                   }
                   argument_list {
                     resolved_literal_node {
                       parent { type { type_kind: TYPE_INT32 } }
                       value {
                         type { type_kind: TYPE_INT32 }
                         value { int32_value: 123 }
                       }
                       has_explicit_type: false
                       float_literal_id: 0
                     }
                   }
                   error_mode: DEFAULT_ERROR_MODE
                 }
                 function_call_info {}
               }
             }
           })pb",
      &request));
  SimpleCatalogProto catalog;

  zetasql::ZetaSQLBuiltinFunctionOptionsProto options;
  zetasql::ZetaSQLBuiltinFunctionOptionsProto* builtin_function_options =
      catalog.mutable_builtin_function_options();
  *builtin_function_options = options;

  *request.mutable_simple_catalog() = catalog;

  BuildSqlResponse response;
  ZETASQL_EXPECT_OK(BuildSql(request, &response));

  BuildSqlResponse expectedResponse;
  expectedResponse.set_sql("foo < (CAST(123 AS INT32))");

  EXPECT_THAT(response, EqualsProto(expectedResponse));
}

TEST_F(ZetaSqlLocalServiceImplTest, FormatSql) {
  FormatSqlRequest request;
  request.set_sql("seLect foo, bar from some_table where something limit 10");

  FormatSqlResponse response;
  ZETASQL_EXPECT_OK(FormatSql(request, &response));

  EXPECT_EQ(
      "SELECT\n"
      "  foo,\n"
      "  bar\n"
      "FROM\n"
      "  some_table\n"
      "WHERE\n"
      "  something\n"
      "LIMIT 10;",
      response.sql());
}

TEST_F(ZetaSqlLocalServiceImplTest, GetBuiltinFunctions) {
  ZetaSQLBuiltinFunctionOptionsProto proto;
  GetBuiltinFunctionsResponse response;
  FunctionProto function1;
  FunctionProto function2;
  google::protobuf::TextFormat::ParseFromString(R"(
      language_options {
        name_resolution_mode: NAME_RESOLUTION_DEFAULT
        product_mode: PRODUCT_INTERNAL
        error_on_deprecated_syntax: false
        supported_statement_kinds: RESOLVED_QUERY_STMT
      }
      include_function_ids: FN_CEIL_DOUBLE
      include_function_ids: FN_EQUAL
      include_function_ids: FN_ANY_VALUE
      exclude_function_ids: FN_ABS_DOUBLE
      exclude_function_ids: FN_ANY_VALUE)",
                                      &proto);
  google::protobuf::TextFormat::ParseFromString(R"(
      name_path: "$equal"
      group: "ZetaSQL"
      mode: SCALAR
      signature {
        argument {
          kind: ARG_TYPE_ANY_1
          options {
            cardinality: REQUIRED
            extra_relation_input_columns_allowed: true
          }
          num_occurrences: -1
        }
        argument {
          kind: ARG_TYPE_ANY_1
          options {
            cardinality: REQUIRED
            extra_relation_input_columns_allowed: true
          }
          num_occurrences: -1
        }
        return_type {
          kind: ARG_TYPE_FIXED
          type {
            type_kind: TYPE_BOOL
          }
          options {
            cardinality: REQUIRED
            extra_relation_input_columns_allowed: true
          }
          num_occurrences: -1
        }
        context_id: 42
        options {
          is_deprecated: false
        }
      }
      options {
        supports_over_clause: false
        window_ordering_support: ORDER_UNSUPPORTED
        supports_window_framing: false
        arguments_are_coercible: true
        is_deprecated: false
        alias_name: ""
        sql_name: "="
        allow_external_usage: true
        volatility: IMMUTABLE
        supports_order_by: false
        supports_limit: false
        supports_null_handling_modifier: false
        supports_safe_error_mode: false
        supports_having_modifier: true
        uses_upper_case_sql_name: true
      })",
                                      &function1);
  google::protobuf::TextFormat::ParseFromString(R"(
      name_path: "ceil"
      group: "ZetaSQL"
      mode: SCALAR
      signature {
        argument {
          kind: ARG_TYPE_FIXED
          type {
            type_kind: TYPE_DOUBLE
          }
          options {
            cardinality: REQUIRED
            extra_relation_input_columns_allowed: true
          }
          num_occurrences: -1
        }
        return_type {
          kind: ARG_TYPE_FIXED
          type {
            type_kind: TYPE_DOUBLE
          }
          options {
            cardinality: REQUIRED
            extra_relation_input_columns_allowed: true
          }
          num_occurrences: -1
        }
        context_id: 1313
        options {
          is_deprecated: false
        }
      }
      options {
        supports_over_clause: false
        window_ordering_support: ORDER_UNSUPPORTED
        supports_window_framing: false
        arguments_are_coercible: true
        is_deprecated: false
        alias_name: "ceiling"
        sql_name: ""
        allow_external_usage: true
        volatility: IMMUTABLE
        supports_order_by: false
        supports_limit: false
        supports_null_handling_modifier: false
        supports_safe_error_mode: true
        supports_having_modifier: true
        uses_upper_case_sql_name: true
      })",
                                      &function2);
  function1.mutable_options()->set_supports_clamped_between_modifier(false);
  function2.mutable_options()->set_supports_clamped_between_modifier(false);

  ASSERT_TRUE(GetBuiltinFunctions(proto, &response).ok());
  EXPECT_EQ(2, response.function_size());
  EXPECT_EQ(function1.DebugString(), response.function(0).DebugString());
  EXPECT_EQ(function2.DebugString(), response.function(1).DebugString());
}

}  // namespace local_service
}  // namespace zetasql
