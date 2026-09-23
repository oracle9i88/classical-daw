#include "audio_unit_runtime.hpp"
#import <Foundation/Foundation.h>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool v, const char* error) { if (!v) throw std::runtime_error(error); }
template<class F> void rejects(F f) {
  bool caught = false; try { f(); } catch (const std::exception&) { caught = true; }
  require(caught, "invalid SWAM state accepted");
}
std::vector<std::uint8_t> state(const std::string& xml, bool cello = true) {
  NSMutableData* raw = [NSMutableData dataWithBytes:"VC2!" length:4];
  unsigned char size[4]{};
  for (unsigned i = 0; i < 4; ++i) size[i] = static_cast<unsigned char>((xml.size() >> (8 * i)) & 255);
  [raw appendBytes:size length:4]; [raw appendBytes:xml.c_str() length:xml.size() + 1];
  NSDictionary* value = @{@"type": @(static_cast<int>('aumu')), @"subtype": @(cello ? 'Sce3' : 'Pt9q'),
      @"manufacturer": @(static_cast<int>('AuMo')), @"jucePluginState": raw};
  NSData* data = [NSPropertyListSerialization dataWithPropertyList:value format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
  const auto* p = static_cast<const std::uint8_t*>([data bytes]); return {p, p + [data length]};
}
std::string xml(const std::string& value, const std::string& range = "36,89") {
  return "<swam range=\"" + range + "\"><program><midimapping><params><PARAM id=\"transpose\" value=\"" + value +
      "\"/></params></midimapping></program></swam>";
}
}
int main() { @autoreleasepool {
  try {
    require(daw::swamCelloStateTranspose(state(xml("-12.0"))) == -12, "negative saved transposition lost");
    require(daw::swamCelloStateTranspose(state(xml("0.0"))) == 0, "concert pitch lost");
    auto text = xml("-12.0");
    text.insert(text.find("</params>"), "<PARAM id=\"unrelated\" value=\"-13.25\"/>");
    const auto original = state(text);
    const auto changed = daw::swamCelloConcertPitchState(original);
    require(daw::swamCelloStateTranspose(original) == -12 && daw::swamCelloStateTranspose(changed) == 0,
            "concert-pitch copy mutated the source or did not update transpose");
    NSData* changed_data = [NSData dataWithBytes:changed.data() length:changed.size()];
    NSDictionary* changed_plist = [NSPropertyListSerialization propertyListWithData:changed_data options:0 format:nil error:nil];
    NSData* raw = changed_plist[@"jucePluginState"];
    NSData* xml_data = [raw subdataWithRange:NSMakeRange(8, [raw length] - 9)];
    NSXMLDocument* doc = [[[NSXMLDocument alloc] initWithData:xml_data options:0 error:nil] autorelease];
    NSArray* others = [doc nodesForXPath:@"/swam/program/midimapping/params/PARAM[@id='unrelated']" error:nil];
    require([others count] == 1 && [[[[others objectAtIndex:0] attributeForName:@"value"] stringValue] isEqualToString:@"-13.25"],
            "concert-pitch edit changed unrelated parameters");
    for (const auto* value : {"", "nan", "0junk", "0.5", "13", "-25"})
      rejects([&] { daw::swamCelloStateTranspose(state(xml(value))); });
    rejects([&] { daw::swamCelloStateTranspose(state(xml("0", "0,127"))); });
    rejects([&] { daw::swamCelloStateTranspose(state(xml("0"), false)); });
    rejects([&] { daw::swamCelloStateTranspose(state("<swam range=\"36,89\"/>")); });
    auto duplicated = xml("0");
    duplicated.insert(duplicated.find("</params>"), "<PARAM id=\"transpose\" value=\"0\"/>");
    rejects([&] { daw::swamCelloStateTranspose(state(duplicated)); });
    rejects([&] { daw::swamCelloStateTranspose(state("<!DOCTYPE swam [<!ENTITY x SYSTEM 'file:///nonexistent'>]>" + xml("0"))); });
    rejects([&] { daw::swamCelloStateTranspose({1, 2, 3}); });
    std::cout << "SWAM state pitch-policy tests passed; no plugins loaded\n"; return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}}
