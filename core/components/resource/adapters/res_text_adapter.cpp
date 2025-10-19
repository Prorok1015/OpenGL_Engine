#include "res_text_adapter.h"

std::shared_ptr<res::text_resource> res::text_adapter::operator()(res::tag tag, const std::vector<std::byte>& data) const
{
	return std::make_shared<res::text_resource>(tag, data);
}