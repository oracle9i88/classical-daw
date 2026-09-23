#include "audio_unit_runtime.hpp"
#import <AppKit/AppKit.h>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>

namespace daw {
int swamCelloStateTranspose(const std::vector<std::uint8_t>& bytes) {
  if (bytes.empty() || bytes.size() > 16U * 1024U * 1024U) throw std::invalid_argument("invalid SWAM state size");
  @autoreleasepool {
    NSData* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    id value = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:nil error:nil];
    if (![value isKindOfClass:[NSDictionary class]]) throw std::invalid_argument("invalid SWAM state dictionary");
    NSDictionary* state = value;
    auto matches = [&](NSString* key, long long expected) {
      id n = [state objectForKey:key]; return [n isKindOfClass:[NSNumber class]] && [n longLongValue] == expected;
    };
    if (!matches(@"type", 'aumu') || !matches(@"subtype", 'Sce3') || !matches(@"manufacturer", 'AuMo'))
      throw std::invalid_argument("state does not belong to SWAM Cello 3");
    NSData* raw = [state objectForKey:@"jucePluginState"];
    if (![raw isKindOfClass:[NSData class]] || [raw length] < 9) throw std::invalid_argument("invalid SWAM JUCE state");
    const auto* b = static_cast<const unsigned char*>([raw bytes]);
    std::uint32_t length = 0;
    for (unsigned i = 0; i < 4; ++i) length |= static_cast<std::uint32_t>(b[4 + i]) << (8 * i);
    if (b[0] != 'V' || b[1] != 'C' || b[2] != '2' || b[3] != '!' ||
        static_cast<std::size_t>(length) + 9 != [raw length] || b[[raw length] - 1] != 0)
      throw std::invalid_argument("unsupported SWAM JUCE XML encoding");
    NSData* xml_data = [NSData dataWithBytes:b + 8 length:length];
    NSString* xml_text = [[[NSString alloc] initWithData:xml_data encoding:NSUTF8StringEncoding] autorelease];
    if (!xml_text || [xml_text rangeOfString:@"<!DOCTYPE" options:NSCaseInsensitiveSearch].location != NSNotFound)
      throw std::invalid_argument("unsupported SWAM state XML");
    NSXMLDocument* xml = [[[NSXMLDocument alloc] initWithData:xml_data options:NSXMLNodeLoadExternalEntitiesNever error:nil] autorelease];
    if (!xml || ![[[[xml rootElement] attributeForName:@"range"] stringValue] isEqualToString:@"36,89"])
      throw std::invalid_argument("unsupported SWAM Cello pitch range");
    NSArray* nodes = [xml nodesForXPath:@"/swam/program/midimapping/params/PARAM[@id='transpose']" error:nil];
    if ([nodes count] != 1) throw std::invalid_argument("missing or ambiguous SWAM transpose setting");
    NSString* attr = [[nodes[0] attributeForName:@"value"] stringValue];
    if (!attr || ![attr UTF8String]) throw std::invalid_argument("invalid SWAM transpose setting");
    std::string text([attr UTF8String]); std::size_t used = 0;
    const double transpose = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(transpose) || std::floor(transpose) != transpose || transpose < -24 || transpose > 12)
      throw std::invalid_argument("unsupported SWAM transposition");
    return static_cast<int>(transpose);
  }
}

void prepareAudioUnitRuntime() {
  if (![NSThread isMainThread]) throw std::logic_error("AU host startup must run on the main thread");
  @autoreleasepool { [NSApplication sharedApplication]; }
}

std::vector<std::uint8_t> swamCelloConcertPitchState(const std::vector<std::uint8_t>& bytes) {
  (void)swamCelloStateTranspose(bytes);  // Validate structure/identity before editing a copy.
  @autoreleasepool {
    NSData* source = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    NSMutableDictionary* state = [NSPropertyListSerialization propertyListWithData:source
        options:NSPropertyListMutableContainersAndLeaves format:nil error:nil];
    NSData* raw = state[@"jucePluginState"];
    NSData* text = [raw subdataWithRange:NSMakeRange(8, [raw length] - 9)];
    NSXMLDocument* xml = [[[NSXMLDocument alloc] initWithData:text options:NSXMLNodeLoadExternalEntitiesNever error:nil] autorelease];
    NSArray* nodes = [xml nodesForXPath:@"/swam/program/midimapping/params/PARAM[@id='transpose']" error:nil];
    [[nodes[0] attributeForName:@"value"] setStringValue:@"0.0"];
    NSData* changed = [xml XMLDataWithOptions:0];
    if (!changed || [changed length] > 16U * 1024U * 1024U) throw std::runtime_error("cannot serialize concert-pitch SWAM XML");
    NSMutableData* payload = [NSMutableData dataWithBytes:"VC2!" length:4];
    unsigned char length[4]{};
    for (unsigned i = 0; i < 4; ++i) length[i] = static_cast<unsigned char>(([changed length] >> (8 * i)) & 255);
    [payload appendBytes:length length:4]; [payload appendData:changed]; const char zero = 0;
    [payload appendBytes:&zero length:1]; state[@"jucePluginState"] = payload;
    NSData* result = [NSPropertyListSerialization dataWithPropertyList:state format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
    if (!result || [result length] > 16U * 1024U * 1024U) throw std::runtime_error("cannot serialize concert-pitch SWAM state");
    const auto* p = static_cast<const std::uint8_t*>([result bytes]);
    return {p, p + [result length]};
  }
}

void serviceAudioUnitRuntime(double seconds) {
  if (![NSThread isMainThread] || !std::isfinite(seconds) || seconds < 0 || seconds > 10) {
    throw std::invalid_argument("invalid AU main-loop startup interval");
  }
  @autoreleasepool {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
      // A run loop with no sources returns immediately. Bound CPU usage as well
      // as wall time while third-party initialization completes.
      [NSThread sleepForTimeInterval:0.01];
    }
  }
}
}
