#pragma once

#if ENABLE(JIT)

#include "JITOperations.h"
#include "PropertyInlineCache.h"
#include "Repatch.h"

// The engine operation a property IC's slow call holds once the IC gives up. Capture compares an IC's slow operation
// with it to set the record's holdsGaveUp bit, and attach writes it into an IC restored as given up. The get, put and
// in access types take it from the helpers their repatch functions use in Repatch.cpp, and every other access type has
// one operation.

namespace JSC::JITCache::ICs {

namespace ICOperationsInternal {

// The operation the repatch functions install when an IC of this access type gives up.
inline CodePtr<CFunctionPtrTag> gaveUpFunction(AccessType accessType)
{
    switch (accessType) {
    case AccessType::GetById:
        return appropriateGetByGaveUpFunction(GetByKind::ById);
    case AccessType::GetByIdWithThis:
        return appropriateGetByGaveUpFunction(GetByKind::ByIdWithThis);
    case AccessType::GetByIdDirect:
        return appropriateGetByGaveUpFunction(GetByKind::ByIdDirect);
    case AccessType::GetByVal:
        return appropriateGetByGaveUpFunction(GetByKind::ByVal);
    case AccessType::GetByValWithThis:
        return appropriateGetByGaveUpFunction(GetByKind::ByValWithThis);
    case AccessType::GetPrivateName:
        return appropriateGetByGaveUpFunction(GetByKind::PrivateName);
    case AccessType::GetPrivateNameById:
        return appropriateGetByGaveUpFunction(GetByKind::PrivateNameById);
    case AccessType::PutByIdStrict:
        return appropriatePutByGaveUpFunction(PutByKind::ByIdStrict);
    case AccessType::PutByIdSloppy:
        return appropriatePutByGaveUpFunction(PutByKind::ByIdSloppy);
    case AccessType::PutByIdDirectStrict:
        return appropriatePutByGaveUpFunction(PutByKind::ByIdDirectStrict);
    case AccessType::PutByIdDirectSloppy:
        return appropriatePutByGaveUpFunction(PutByKind::ByIdDirectSloppy);
    case AccessType::PutByValStrict:
        return appropriatePutByGaveUpFunction(PutByKind::ByValStrict);
    case AccessType::PutByValSloppy:
        return appropriatePutByGaveUpFunction(PutByKind::ByValSloppy);
    case AccessType::PutByValDirectStrict:
        return appropriatePutByGaveUpFunction(PutByKind::ByValDirectStrict);
    case AccessType::PutByValDirectSloppy:
        return appropriatePutByGaveUpFunction(PutByKind::ByValDirectSloppy);
    case AccessType::DefinePrivateNameByVal:
        return appropriatePutByGaveUpFunction(PutByKind::DefinePrivateNameByVal);
    case AccessType::DefinePrivateNameById:
        return appropriatePutByGaveUpFunction(PutByKind::DefinePrivateNameById);
    case AccessType::SetPrivateNameByVal:
        return appropriatePutByGaveUpFunction(PutByKind::SetPrivateNameByVal);
    case AccessType::SetPrivateNameById:
        return appropriatePutByGaveUpFunction(PutByKind::SetPrivateNameById);
    case AccessType::InById:
        return appropriateInByGaveUpFunction(InByKind::ById);
    case AccessType::InByVal:
        return appropriateInByGaveUpFunction(InByKind::ByVal);
    case AccessType::HasPrivateName:
        return appropriateInByGaveUpFunction(InByKind::PrivateName);
    case AccessType::HasPrivateBrand:
        return operationHasPrivateBrandGaveUp;
    case AccessType::InstanceOf:
        return operationInstanceOfGaveUp;
    case AccessType::DeleteByIdStrict:
        return operationDeleteByIdStrictGaveUp;
    case AccessType::DeleteByIdSloppy:
        return operationDeleteByIdSloppyGaveUp;
    case AccessType::DeleteByValStrict:
        return operationDeleteByValStrictGaveUp;
    case AccessType::DeleteByValSloppy:
        return operationDeleteByValSloppyGaveUp;
    case AccessType::CheckPrivateBrand:
        return operationCheckPrivateBrandGaveUp;
    case AccessType::SetPrivateBrand:
        return operationSetPrivateBrandGaveUp;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace ICOperationsInternal

// The *GaveUp operation a property IC of this access type holds once it gives up, tagged as repatchSlowPathCall stores
// it into HandlerPropertyInlineCache::m_slowOperation.
inline CodePtr<OperationPtrTag> gaveUpOperationFor(AccessType accessType)
{
    return ICOperationsInternal::gaveUpFunction(accessType).retagged<OperationPtrTag>();
}

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JIT)
