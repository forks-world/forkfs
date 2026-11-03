#import <Foundation/Foundation.h>
NSDictionary* RevisionLoadDescriptor(NSURL* runtime,NSError** error);
NSUUID* RevisionDescriptorUUID(NSDictionary* config);
