/* Display output is configured by the launcher through dgVoodoo.conf.
 * Keep the game's original mode table and startup code untouched: HGL's
 * hardware enumeration rejects resolutions absent from its device mode list.
 */
void tod2p_log(const char *fmt, ...);
void tod2p_display_init(int mode)
{
    tod2p_log("display: mode %d handled by dgVoodoo; HGL stays at original 640x480", mode);
}
int tod2p_display_active(void) { return 0; }
