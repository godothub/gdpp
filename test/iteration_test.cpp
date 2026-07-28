#include "support/test.hpp"

#include "gdpp/semantic/iteration.hpp"
#include "gdpp/semantic/type.hpp"

TEST_CASE("generic container arguments preserve nested type boundaries") {
    const auto array =
        gdpp::generic_type_arguments("Array[Dictionary[String, Array[int]]]", "Array", 1);
    const auto dictionary = gdpp::generic_type_arguments(
        "Dictionary[String, Dictionary[int, Array[String]]]", "Dictionary", 2);

    REQUIRE(array.has_value());
    REQUIRE_EQ(array->size(), std::size_t{1});
    REQUIRE_EQ(array->front(), std::string{"Dictionary[String, Array[int]]"});
    REQUIRE(dictionary.has_value());
    REQUIRE_EQ(dictionary->size(), std::size_t{2});
    REQUIRE_EQ(dictionary->at(0), std::string{"String"});
    REQUIRE_EQ(dictionary->at(1), std::string{"Dictionary[int, Array[String]]"});
    REQUIRE(!gdpp::generic_type_arguments("Dictionary[String]", "Dictionary", 2));
    REQUIRE(!gdpp::generic_type_arguments("Array[String, int]", "Array", 1));
}

TEST_CASE("typed container descriptors preserve source constraints") {
    const auto array =
        gdpp::describe_container_type(gdpp::type_from_annotation("Array[Dictionary[String, int]]"));
    const auto dictionary =
        gdpp::describe_container_type(gdpp::type_from_annotation("Dictionary[Variant, Variant]"));

    REQUIRE(array.has_value());
    REQUIRE(array->kind == gdpp::ContainerTypeKind::array);
    REQUIRE_EQ(array->arguments.at(0), std::string{"Dictionary[String, int]"});
    REQUIRE(array->has_runtime_constraint());
    REQUIRE(dictionary.has_value());
    REQUIRE_EQ(dictionary->kind, gdpp::ContainerTypeKind::dictionary);
    REQUIRE_EQ(dictionary->arguments.size(), std::size_t{2});
    REQUIRE(!dictionary->has_runtime_constraint());
    REQUIRE(gdpp::is_explicitly_typed_container(gdpp::type_from_annotation("Array[Variant]")));
    REQUIRE(!gdpp::is_explicitly_typed_container(gdpp::type_from_annotation("Array")));
    REQUIRE(!gdpp::describe_container_type(gdpp::type_from_annotation("Dictionary")));
}

TEST_CASE("iteration plans classify every accepted iterable family") {
    const gdpp::Type integer{gdpp::TypeKind::integer, "int"};
    const gdpp::Type enumeration{gdpp::TypeKind::enumeration, "State"};
    const gdpp::Type floating{gdpp::TypeKind::floating, "float"};
    const gdpp::Type vector2{gdpp::TypeKind::builtin, "Vector2"};
    const gdpp::Type vector2i{gdpp::TypeKind::builtin, "Vector2i"};
    const gdpp::Type vector3{gdpp::TypeKind::builtin, "Vector3"};
    const gdpp::Type vector3i{gdpp::TypeKind::builtin, "Vector3i"};
    const gdpp::Type string{gdpp::TypeKind::string, "String"};
    const gdpp::Type array{gdpp::TypeKind::array, "Array[int]"};
    const gdpp::Type dictionary{gdpp::TypeKind::dictionary, "Dictionary[String, int]"};
    const gdpp::Type packed{gdpp::TypeKind::builtin, "PackedInt64Array"};
    const gdpp::Type dynamic{gdpp::TypeKind::variant, "Variant"};
    const gdpp::Type object{gdpp::TypeKind::object, "CustomIterator"};

    REQUIRE_EQ(gdpp::make_iteration_plan(integer, integer, false).strategy,
               gdpp::IterationStrategy::integer_count);
    REQUIRE_EQ(gdpp::make_iteration_plan(enumeration, integer, false).strategy,
               gdpp::IterationStrategy::integer_count);
    REQUIRE_EQ(gdpp::make_iteration_plan(floating, floating, false).strategy,
               gdpp::IterationStrategy::floating_count);
    REQUIRE_EQ(gdpp::make_iteration_plan(vector2, floating, false).strategy,
               gdpp::IterationStrategy::vector2_range);
    REQUIRE_EQ(gdpp::make_iteration_plan(vector2i, integer, false).strategy,
               gdpp::IterationStrategy::vector2i_range);
    REQUIRE_EQ(gdpp::make_iteration_plan(vector3, floating, false).strategy,
               gdpp::IterationStrategy::vector3_range);
    REQUIRE_EQ(gdpp::make_iteration_plan(vector3i, integer, false).strategy,
               gdpp::IterationStrategy::vector3i_range);
    REQUIRE_EQ(gdpp::make_iteration_plan(string, string, false).strategy,
               gdpp::IterationStrategy::indexed_string);
    REQUIRE_EQ(gdpp::make_iteration_plan(array, integer, false).strategy,
               gdpp::IterationStrategy::indexed_array);
    REQUIRE_EQ(gdpp::make_iteration_plan(dictionary, string, false).strategy,
               gdpp::IterationStrategy::dictionary_protocol);
    REQUIRE_EQ(gdpp::make_iteration_plan(packed, integer, false).strategy,
               gdpp::IterationStrategy::indexed_packed_array);
    REQUIRE_EQ(gdpp::make_iteration_plan(dynamic, dynamic, false).strategy,
               gdpp::IterationStrategy::dynamic_protocol);
    REQUIRE_EQ(gdpp::make_iteration_plan(object, dynamic, false).strategy,
               gdpp::IterationStrategy::object_protocol);
    REQUIRE_EQ(gdpp::make_iteration_plan(array, integer, true).strategy,
               gdpp::IterationStrategy::intrinsic_range);
}
