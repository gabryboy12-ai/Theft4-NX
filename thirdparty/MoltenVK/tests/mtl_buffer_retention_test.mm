// Focused ownership tests; no GPU work or game execution is required.
#include "MVKMTLBufferRetention.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(condition) do { \
	if (!(condition)) { \
		std::fprintf(stderr, "CHECK failed: %s at line %d\n", #condition, __LINE__); \
		std::abort(); \
	} \
} while (false)

@interface RetentionTestBuffer : NSObject {
	bool* _destroyed;
}
- (instancetype)initWithDestroyedFlag:(bool*)destroyed;
@end

@implementation RetentionTestBuffer
- (instancetype)initWithDestroyedFlag:(bool*)destroyed {
	if ((self = [super init])) { _destroyed = destroyed; }
	return self;
}
- (void)dealloc {
	*_destroyed = true;
	[super dealloc];
}
@end

// Only the completion-handler contract is needed by the production helper.
// Keep handlers after completion to model a Metal wrapper surviving execution.
@interface RetentionTestCommandBuffer : NSObject {
	NSMutableArray* _handlers;
	bool* _destroyed;
	MTLCommandBufferStatus _status;
}
- (instancetype)initWithDestroyedFlag:(bool*)destroyed;
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler;
- (void)completeWithStatus:(MTLCommandBufferStatus)status;
- (NSUInteger)handlerCount;
- (MTLCommandBufferStatus)status;
@end

@implementation RetentionTestCommandBuffer
- (instancetype)initWithDestroyedFlag:(bool*)destroyed {
	if ((self = [super init])) {
		_destroyed = destroyed;
		_handlers = [NSMutableArray new];
		_status = MTLCommandBufferStatusNotEnqueued;
	}
	return self;
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {
	id copied = [handler copy];
	[_handlers addObject:copied];
	[copied release];
}
- (void)completeWithStatus:(MTLCommandBufferStatus)status {
	_status = status;
	for (MTLCommandBufferHandler handler in _handlers) {
		handler((id<MTLCommandBuffer>)self);
	}
}
- (NSUInteger)handlerCount { return _handlers.count; }
- (MTLCommandBufferStatus)status { return _status; }
- (void)dealloc {
	[_handlers release];
	*_destroyed = true;
	[super dealloc];
}
@end

static void testDeduplicationAndCompletion() {
	bool commandDestroyed = false;
	bool bufferDestroyed = false;
	auto* command = [[RetentionTestCommandBuffer alloc] initWithDestroyedFlag:&commandDestroyed];
	auto* buffer = [[RetentionTestBuffer alloc] initWithDestroyedFlag:&bufferDestroyed];
	auto* firstEncoder = mvkGetMTLBufferRetention((id<MTLCommandBuffer>)command);
	firstEncoder->retainBuffer((id<MTLBuffer>)buffer);
	firstEncoder->retainBuffer(nil);
	// A different Vulkan encoder/pass appending to a prefilled Metal CB shares ownership.
	auto* nextEncoder = mvkGetMTLBufferRetention((id<MTLCommandBuffer>)command);
	CHECK(firstEncoder == nextEncoder);
	nextEncoder->retainBuffer((id<MTLBuffer>)buffer);
	CHECK([command handlerCount] == 1);
	[buffer release];
	CHECK(!bufferDestroyed);
	[command completeWithStatus:MTLCommandBufferStatusCompleted];
	CHECK(bufferDestroyed);
	CHECK(!commandDestroyed);
	// Completion cleanup is idempotent, and dealloc cleanup must not release twice.
	[command completeWithStatus:MTLCommandBufferStatusCompleted];
	[command release];
	CHECK(commandDestroyed);
}

static void testOverlappingCommandBuffersAndError() {
	bool firstDestroyed = false;
	bool nextDestroyed = false;
	bool bufferDestroyed = false;
	auto* first = [[RetentionTestCommandBuffer alloc] initWithDestroyedFlag:&firstDestroyed];
	auto* next = [[RetentionTestCommandBuffer alloc] initWithDestroyedFlag:&nextDestroyed];
	auto* buffer = [[RetentionTestBuffer alloc] initWithDestroyedFlag:&bufferDestroyed];
	mvkGetMTLBufferRetention((id<MTLCommandBuffer>)first)->retainBuffer((id<MTLBuffer>)buffer);
	mvkGetMTLBufferRetention((id<MTLCommandBuffer>)next)->retainBuffer((id<MTLBuffer>)buffer);
	[buffer release];
	[first completeWithStatus:MTLCommandBufferStatusCompleted];
	CHECK(!bufferDestroyed);
	[first release];
	CHECK(firstDestroyed);
	CHECK(!bufferDestroyed);
	[next completeWithStatus:MTLCommandBufferStatusError];
	CHECK(bufferDestroyed);
	[next release];
	CHECK(nextDestroyed);
}

static void testDiscardWithoutCompletion() {
	bool commandDestroyed = false;
	bool firstDestroyed = false;
	bool secondDestroyed = false;
	auto* command = [[RetentionTestCommandBuffer alloc] initWithDestroyedFlag:&commandDestroyed];
	auto* first = [[RetentionTestBuffer alloc] initWithDestroyedFlag:&firstDestroyed];
	auto* second = [[RetentionTestBuffer alloc] initWithDestroyedFlag:&secondDestroyed];
	auto* retention = mvkGetMTLBufferRetention((id<MTLCommandBuffer>)command);
	retention->retainBuffer((id<MTLBuffer>)first);
	retention->retainBuffer((id<MTLBuffer>)second);
	[first release];
	[second release];
	CHECK(!firstDestroyed && !secondDestroyed);
	[command release];
	CHECK(commandDestroyed && firstDestroyed && secondDestroyed);
}

int main() {
	@autoreleasepool {
		CHECK(mvkGetMTLBufferRetention(nil) == nullptr);
		testDeduplicationAndCompletion();
		testOverlappingCommandBuffersAndError();
		testDiscardWithoutCompletion();
	}
	std::puts("Metal buffer retention ownership tests passed");
	return 0;
}
