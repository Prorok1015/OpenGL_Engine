#pragma once

namespace edt {

	// Which changes of the game layer the editor follows while the game module is reloaded (#435). A swap changes the
	// layer twice (module -> empty -> new module, or back to the old one): the level is detached at the first change and
	// attached again only at the end, to whichever layer is the game layer then, never built in the empty one between.
	class module_swap_gate
	{
	public:
		void begin() { m_swapping = true; }

		// Whether the editor handles this change of the game layer now.
		bool pass(bool before)
		{
			if (!m_swapping) {
				return true;
			}
			if (!before || m_detached) {
				return false;
			}
			m_detached = true;
			return true;
		}

		// Whether the attach held back during the swap is due now.
		bool end()
		{
			m_swapping = false;
			const bool detached = m_detached;
			m_detached = false;
			return detached;
		}

	private:
		bool m_swapping = false;
		bool m_detached = false;
	};

}
