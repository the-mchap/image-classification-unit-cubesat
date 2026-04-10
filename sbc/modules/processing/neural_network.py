"""TFLite inference engine. Loads a quantized image classification model and runs predictions."""

import os
from typing import Tuple
import numpy as np
import tflite_runtime.interpreter as tflite
from .. import config

logger = config.logging.get_logger(__name__)


class NeuralNetwork:
    """
    A class to manage loading the TFLite model and running predictions.
    """

    def __init__(self, model_path: str, input_width: int, input_height: int):
        """
        Initializes the neural network, loading the TFLite model.
        """
        self.interpreter = self._load_model(model_path)
        self.input_width = input_width
        self.input_height = input_height
        self.labels = ["LAND", "SKY", "WATER", "BAD"]

    def _load_model(self, model_path: str) -> tflite.Interpreter:
        """Loads the TFLite model and returns the configured interpreter."""
        if not os.path.exists(model_path):
            logger.critical(f"TFLite model {model_path} not found. Aborting.")
            raise FileNotFoundError(f"TFLite model {model_path} not found.")
        try:
            interpreter = tflite.Interpreter(model_path=model_path)
            interpreter.allocate_tensors()
            logger.info(f"TFLite model loaded from {model_path}")
            return interpreter
        except Exception as e:
            logger.critical(f"Fatal error loading TFLite model: {e}")
            raise e

    def predict(self, lores_image_array: np.ndarray) -> Tuple[str, float]:
        """
        Performs prediction using the pre-resized lores image.
        Returns (classification, confidence).
        """
        try:
            input_details = self.interpreter.get_input_details()
            output_details = self.interpreter.get_output_details()

            # Normalize and expand dims for TFLite
            img = np.expand_dims(lores_image_array, axis=0).astype(np.float32) / 255.0

            self.interpreter.set_tensor(input_details[0]["index"], img)
            self.interpreter.invoke()

            output_data = self.interpreter.get_tensor(output_details[0]["index"])
            class_idx = np.argmax(output_data)
            confidence = float(output_data[0][class_idx])

            if class_idx >= len(self.labels):
                logger.error(f"Class index ({class_idx}) out of range. Using 'BAD'.")
                return config.app.INVALID_IMAGE_CLASS, 0.0

            classification = self.labels[class_idx]
            logger.info(
                f"TFLite prediction: {classification} (Confidence: {confidence:.2f})"
            )
            return classification, confidence

        except Exception as e:
            logger.error(f"Error in TFLite prediction (invoke): {e}")
            return config.app.INVALID_IMAGE_CLASS, 0.0
