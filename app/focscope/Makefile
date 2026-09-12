############################################################################
# apps/examples/focscope/Makefile
#
# FOCPilot oscilloscope UI example
#
############################################################################

include $(APPDIR)/Make.defs

# FOC oscilloscope main app (UI + CAN telemetry + AI tune page)

ifneq ($(CONFIG_EXAMPLES_FOCSCOPE),)
PROGNAME  = focscope
PRIORITY  = $(CONFIG_EXAMPLES_FOCSCOPE_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_FOCSCOPE_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_FOCSCOPE)

MAINSRC = focscope.c
CSRCS   = scope_ui.c scope_data_sim.c scope_data_can.c font_puhui_20_4.c \
          ai_tuner_ui.c foc_ai_tuner.c wifi_ui.c
endif

# AI Tuner test program (command-line, shares foc_ai_tuner.c)

ifneq ($(CONFIG_EXAMPLES_FOCSCOPE_AI_TEST),)
PROGNAME  += ai_tuner_test
PRIORITY  += $(CONFIG_EXAMPLES_FOCSCOPE_AI_TEST_PRIORITY)
STACKSIZE += $(CONFIG_EXAMPLES_FOCSCOPE_AI_TEST_STACKSIZE)
MODULE    += $(CONFIG_EXAMPLES_FOCSCOPE_AI_TEST)

MAINSRC += ai_tuner_test.c
endif

# AI Tuner CAN app (CAN-based tuning, shares foc_ai_tuner.c)

ifneq ($(CONFIG_EXAMPLES_FOCSCOPE_AI_TUNER_CAN),)
PROGNAME  += ai_tuner_can
PRIORITY  += $(CONFIG_EXAMPLES_FOCSCOPE_AI_TUNER_CAN_PRIORITY)
STACKSIZE += $(CONFIG_EXAMPLES_FOCSCOPE_AI_TUNER_CAN_STACKSIZE)
MODULE    += $(CONFIG_EXAMPLES_FOCSCOPE_AI_TUNER_CAN)

MAINSRC += ai_tuner_can.c
endif

# foc_ai_tuner.c must also be available to ai_tuner_test / ai_tuner_can
# when focscope itself is not enabled.

ifneq ($(CONFIG_EXAMPLES_FOCSCOPE),)
else
ifneq ($(CONFIG_EXAMPLES_FOCSCOPE_AI_TEST)$(CONFIG_EXAMPLES_FOCSCOPE_AI_TUNER_CAN),)
CSRCS   += foc_ai_tuner.c
endif
endif

include $(APPDIR)/Application.mk
