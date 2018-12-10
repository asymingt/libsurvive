#include <assert.h>
#include <math.h>
#include <survive.h>

bool SurviveSensorActivations_isReadingValid(const SurviveSensorActivations *self, survive_timecode tolerance,
											 survive_timecode timecode_now, uint32_t idx, int lh, int axis) {
	const uint32_t *data_timecode = self->timecode[idx][lh];
	if (self->lengths[idx][lh][axis] == 0)
		return false;

	return survive_timecode_difference(timecode_now, data_timecode[axis]) <= tolerance;
}
bool SurviveSensorActivations_isPairValid(const SurviveSensorActivations *self, uint32_t tolerance,
										  uint32_t timecode_now, uint32_t idx, int lh) {
	const uint32_t *data_timecode = self->timecode[idx][lh];
	if (self->lengths[idx][lh][0] == 0 || self->lengths[idx][lh][1] == 0)
		return false;

	return !(timecode_now - data_timecode[0] > tolerance || timecode_now - data_timecode[1] > tolerance);
}

void SurviveSensorActivations_add_imu(SurviveSensorActivations *self, struct PoserDataIMU *imuData) {
	self->last_imu = imuData->timecode;
	for (int i = 0; i < 3; i++) {
		self->accel[i] = .98 * self->accel[i] + .02 * imuData->accel[i];
	}
	for (int i = 0; i < 3; i++) {
		self->gyro[i] = .98 * self->gyro[i] + .02 * imuData->gyro[i];
	}
	for (int i = 0; i < 3; i++) {
		self->mag[i] = .98 * self->mag[i] + .02 * imuData->mag[i];
	}
}
void SurviveSensorActivations_add(SurviveSensorActivations *self, struct PoserDataLight *lightData) {
	int axis = (lightData->acode & 1);
	uint32_t *data_timecode = &self->timecode[lightData->sensor_id][lightData->lh][axis];

	FLT *angle = &self->angles[lightData->sensor_id][lightData->lh][axis];
	uint32_t *length = &self->lengths[lightData->sensor_id][lightData->lh][axis];
	// assert(*length == 0 || fabs(*angle - lightData->angle) < 0.05);

	*angle = lightData->angle;
	*data_timecode = lightData->timecode;
	*length = (uint32_t)(lightData->length * 48000000);
}

SURVIVE_EXPORT uint32_t SurviveSensorActivations_default_tolerance = (uint32_t)(48000000 /*mhz*/ * (16.7 * 2 /*ms*/) / 1000) + 5000;
