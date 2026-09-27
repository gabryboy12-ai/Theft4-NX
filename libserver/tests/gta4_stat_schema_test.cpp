#include "gta4_stat_schema.h"

#include <cstdlib>
#include <iostream>

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) std::cerr << "gta4_stat_schema_test: " << message << '\n';
  return condition;
}

bool SameMapping(const libserver::Gta4StatAttributeMapping* left,
                 const libserver::Gta4StatAttributeMapping* right) {
  return left && right && left->view_id == right->view_id &&
         left->attribute_id == right->attribute_id && left->property_id == right->property_id;
}

}  // namespace

int main() {
  using namespace libserver;

  bool passed = Check(ValidateGta4StatSchema(), "generated schema validation failed");
  const auto mappings = Gta4ExtractedStatAttributeMappings();
  passed &= Check(mappings.size() == kGta4ExtractedStatAttributeMappingCount,
                  "attribute mapping count changed");
  passed &= Check(mappings.size() == Gta4ExtractedStatFields().size(),
                  "field and attribute mapping counts differ");

  for (const auto& mapping : mappings) {
    const auto* by_id = FindGta4StatAttributeById(mapping.view_id, mapping.attribute_id);
    const auto* by_property =
        FindGta4StatAttributeByPropertyId(mapping.view_id, mapping.property_id);
    passed &= Check(SameMapping(by_id, &mapping), "attribute-id lookup is not reciprocal");
    passed &= Check(SameMapping(by_property, &mapping),
                    "property-id lookup is not reciprocal");
  }

  for (const auto& field : Gta4ExtractedStatFields()) {
    const auto* mapping = FindGta4StatAttributeByPropertyId(field.view_id, field.property_id);
    passed &= Check(mapping && mapping->view_id == field.view_id &&
                        mapping->property_id == field.property_id,
                    "extracted field has no attribute mapping");
  }

  const auto* race_best_lap = FindGta4StatAttributeById(1u, UINT16_C(0x0003));
  passed &= Check(race_best_lap && race_best_lap->property_id == UINT32_C(0x2000002C),
                  "view 1 best-lap mapping changed");
  const auto* race_rank = FindGta4StatAttributeById(1u, UINT16_C(0xFFFF));
  passed &= Check(race_rank && race_rank->property_id == UINT32_C(0x10008001),
                  "view 1 rank mapping changed");
  const auto* ranked_cash =
      FindGta4StatAttributeByPropertyId(109u, UINT32_C(0x2000000D));
  passed &= Check(ranked_cash && ranked_cash->attribute_id == UINT16_C(0xFFFE),
                  "view 109 cash mapping changed");

  passed &= Check(FindGta4StatAttributeById(0u, UINT16_C(0x0001)) == nullptr,
                  "unknown view unexpectedly resolved");
  passed &= Check(FindGta4StatAttributeById(1u, UINT16_C(0x0000)) == nullptr,
                  "unknown attribute unexpectedly resolved");
  passed &= Check(
      FindGta4StatAttributeByPropertyId(109u, UINT32_C(0x2000003E)) == nullptr,
      "internal compatibility property unexpectedly has a trusted XLAST attribute mapping");

  if (passed) std::cout << "gta4_stat_schema_test passed\n";
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
