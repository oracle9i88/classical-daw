#pragma once
#include "performance_audition.hpp"
#include "ensemble_audition.hpp"
#include "daw/document_audition.hpp"

namespace daw {
using DocumentAudition = BasicDocumentAudition<PerformanceAudition, EnsembleAudition>;
}
