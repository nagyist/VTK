## Fix vtkHyperTreeGridEvaluateCoarse performance

Fixed multithreaded `vtkHyperTreeGridEvaluateCoarse` execution race condition, and improved performance for some datasets by multi-threading only at tree level and not below.
