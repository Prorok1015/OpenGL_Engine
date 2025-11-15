#pragma once
#include "gui_system.h"
#define DBG_UI_REG_LAMBDA(_path_, ...)            \
   {                                              \
      gui::get_system().registrate_menu_dbg(_path_, __VA_ARGS__); \
   }

#define DBG_UI_UNREG(_path_)           \
   {                                   \
      gui::get_system().unregistrate_menu_dbg(_path_); \
   }


#define DBG_UI_SET_ITEM_CHECKED(_path_, _val_)    \
   {                                              \
      gui::get_system().set_menu_checked_dbg(_path_, _val_); \
   }

#define DBG_UI_IS_ITEM_CHECKED(_path_) gui::get_system().is_item_checked_dbg(_path_)
#define DBG_UI_MENU_ITEM_CHECK_LAMBDA(_path_, ...)        \
   {                                                      \
      gui::get_system().set_check_callback_dbg(_path_, __VA_ARGS__); \
   }

#define DBG_UI_REG_LAMBDA_IMPLICIT(_path_, ...)           \
   {                                                      \
      gui::get_system().register_implicit_dbg(_path_, __VA_ARGS__); \
   }
#define DBG_UI_UNREG_IMPLICIT(_path_)          \
   {                                           \
      gui::get_system().unregister_implicit_dbg(_path_); \
   }



#define GUI_REG_LAMBDA(_path_, ...)                                 \
   {                                                                \
      gui::get_system().registrate_menu(_path_, __VA_ARGS__);    \
   }

#define GUI_UNREG(_path_)                                           \
   {                                                                \
      gui::get_system().unregistrate_menu(_path_);               \
   }


#define GUI_SET_ITEM_CHECKED(_path_, _val_)                         \
   {                                                                \
      gui::get_system().set_menu_checked(_path_, _val_);         \
   }

#define GUI_IS_ITEM_CHECKED(_path_) gui::get_system().is_item_checked(_path_)
#define GUI_MENU_ITEM_CHECK_LAMBDA(_path_, ...)                     \
   {                                                                \
      gui::get_system().set_check_callback(_path_, __VA_ARGS__); \
   }

#define GUI_REG_LAMBDA_IMPLICIT(_path_, ...)                        \
   {                                                                \
      gui::get_system().register_implicit(_path_, __VA_ARGS__);  \
   }
#define GUI_UNREG_IMPLICIT(_path_)                                  \
   {                                                                \
      gui::get_system().unregister_implicit(_path_);             \
   }