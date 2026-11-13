//! Dense float32 tensors passed between the segmentation engine and the model backends.

use anyhow::{Result, bail};

/// A row-major float32 tensor. The data length always equals the product of the shape.
#[derive(Debug, Clone, PartialEq)]
pub struct TensorF32 {
    shape: Vec<usize>,
    data: Vec<f32>,
}

impl TensorF32 {
    /// A tensor of @p shape with @p data in row-major order. Fails when the data length differs
    /// from the product of the shape.
    pub fn new(shape: Vec<usize>, data: Vec<f32>) -> Result<Self> {
        let expected: usize = shape.iter().product();
        if expected != data.len() {
            bail!(
                "tensor shape {:?} needs {} values, got {}",
                shape,
                expected,
                data.len()
            );
        }
        Ok(Self { shape, data })
    }

    /// A tensor of @p shape filled with zeros.
    pub fn zeros(shape: Vec<usize>) -> Self {
        let len = shape.iter().product();
        Self {
            shape,
            data: vec![0.0; len],
        }
    }

    pub fn shape(&self) -> &[usize] {
        &self.shape
    }

    pub fn data(&self) -> &[f32] {
        &self.data
    }

    pub fn into_data(self) -> Vec<f32> {
        self.data
    }

    /// Fails unless the shape equals @p expected. @p name identifies the tensor in the error.
    pub fn expect_shape(&self, name: &str, expected: &[usize]) -> Result<()> {
        if self.shape != expected {
            bail!("{name} has shape {:?}, expected {:?}", self.shape, expected);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tensor_rejects_data_length_that_differs_from_shape() {
        let err = TensorF32::new(vec![2, 3], vec![0.0; 5]).expect_err("length mismatch");
        assert!(err.to_string().contains("needs 6 values, got 5"), "{err}");
        let tensor = TensorF32::new(vec![2, 3], vec![1.0; 6]).expect("valid tensor");
        assert_eq!(tensor.shape(), &[2, 3]);
        assert!(tensor.expect_shape("x", &[3, 2]).is_err());
    }
}
